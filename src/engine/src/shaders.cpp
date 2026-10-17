#include "shaders.h"

#include <d3dcompiler.h>

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ff7vr::engine::shaders {
namespace {

constexpr std::size_t kCreatePixelShader = 15;  // ID3D11Device vtable slot

struct Entry {
    Hash hash{};      // the game's shader (also when a patched copy was created in its place)
    Hash run_hash{};  // the bytecode the object was created from
    std::size_t size = 0;
    std::vector<std::uint8_t> code;  // empty unless kept; the bytecode the object was created from
};

std::mutex g_mutex;
std::unordered_map<void*, Entry>* g_seen = new std::unordered_map<void*, Entry>();  // never freed (shutdown order)
std::set<Hash>* g_wanted = new std::set<Hash>();
std::atomic<bool> g_keep_all{false};
std::atomic<std::uint64_t> g_created{0}, g_kept_bytes{0};
hook::InlineHook* g_hook = new hook::InlineHook();

// Shaders that add the view rectangle's origin to SV_Position (see find_view_origin_add) and
// their patched copies, by the engine's shader object. Under g_mutex.
struct Patched {
    ID3D11PixelShader* shader = nullptr;  // referenced
    Hash hash{};
};
std::unordered_map<void*, Patched>* g_patched = new std::unordered_map<void*, Patched>();
std::set<Hash>* g_patched_logged = new std::set<Hash>();
std::atomic<std::uint64_t> g_patch_matches{0}, g_patch_failures{0};

// The dither patch (see find_dither): the size of the static tile (0: as the game), whether
// the full-screen form is patched too, and every distinct shader seen, by its 3DMigoto hash
// (`shader list`). Under g_mutex.
std::atomic<int> g_dither_size{32};
std::atomic<bool> g_dither_screen{false};
std::atomic<std::uint64_t> g_dither_patched{0}, g_dither_failed{0}, g_dither_material_seen{0}, g_dither_screen_seen{0};
struct Known {
    Hash hash{};
    std::size_t size = 0;
    unsigned material = 0, screen = 0;
    bool patched = false;
};
std::map<std::uint64_t, Known>* g_known = new std::map<std::uint64_t, Known>();

// Comparison mode ([dev] dither_ab = 1): every dithered shader is also created as the game's
// and in each tile size, and a hook on PSSetShader binds the variant `shader dither <mode>`
// selects, so the modes can be compared in one session. Under g_mutex.
// Variant index -> tile (0: the game's shader). A negative value -k is the "slow" form: the
// game's full noise, but the slice advances only every 2^k frames (see slow_copy).
constexpr int kTiles[] = {0, 2, 4, 8, 16, 32, 64, -2, -3, -4, -5};
constexpr std::size_t kVariants = std::size(kTiles);
std::atomic<bool> g_ab{false};
std::atomic<int> g_ab_variant{-1};  // -1: none, the object as created
std::unordered_map<void*, std::array<ID3D11PixelShader*, kVariants>>* g_variants =
    new std::unordered_map<void*, std::array<ID3D11PixelShader*, kVariants>>();
hook::InlineHook* g_set_hook = new hook::InlineHook();
std::atomic<std::uint64_t> g_ab_swaps{0};
constexpr std::size_t kPSSetShader = 9;  // ID3D11DeviceContext vtable slot
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

Hash hash_of(const void* code, SIZE_T size) {
    Hash h{};
    if (code && size >= 20 && std::memcmp(code, "DXBC", 4) == 0) std::memcpy(h.data(), static_cast<const std::uint8_t*>(code) + 4, 16);
    return h;
}

// ------------------------------------------------------------------ the view-origin patch
// A pixel shader drawn over the whole double-wide target that reads its inputs at
// SV_Position + ViewRectMin while it derives the screen position from SV_Position -
// ViewRectMin can only be right for a view at the origin (the first is correct for a pixel
// position relative to the view, the second for one relative to the target). Square Enix's
// screen-space reflections do this (docs/engine-module.md, "Reflections per eye"). The patch
// makes the first one add ViewRectMin.zzzz, which is 0 (UE 4.18 fills that vector as x, y, 0,
// 0), so every position in the shader is relative to the target and the pass is right for a
// view drawn at its own rectangle; for a view at the origin nothing changes.

struct Operand {
    std::size_t pos = 0;  // dword index of the operand token
    std::uint32_t token = 0;
    unsigned type = 0, dim = 0, modifier = 0;
    std::uint32_t index[3]{};
    bool immediate_indices = true;
    std::size_t imm = 0;  // dword index of the first immediate value (immediate32 operands)
};

// The components an operand writes (mask mode) or the first one it reads (swizzle, select-1).
unsigned write_mask(const Operand& o) { return ((o.token >> 2) & 3) == 0 ? (o.token >> 4) & 0xf : 0; }
unsigned first_component(const Operand& o) {
    const unsigned mode = (o.token >> 2) & 3;
    return mode == 1 || mode == 2 ? (o.token >> 4) & 3 : 0;
}

// Decodes one operand at tok[i]; returns the index after it, or 0 if it does not fit.
std::size_t decode_operand(const std::uint32_t* tok, std::size_t n, std::size_t i, Operand& o) {
    if (i >= n) return 0;
    o = Operand{};
    o.pos = i;
    o.token = tok[i++];
    const unsigned ncomp = o.token & 3;
    o.type = (o.token >> 12) & 0xff;
    o.dim = (o.token >> 20) & 3;
    bool ext = (o.token >> 31) != 0;
    while (ext) {
        if (i >= n) return 0;
        const std::uint32_t e = tok[i++];
        if ((e & 0x3f) == 1) o.modifier = (e >> 6) & 0xff;
        ext = (e >> 31) != 0;
    }
    if (o.type == 4) {  // immediate32
        o.imm = i;
        i += ncomp == 2 ? 4 : 1;
    }
    if (o.type == 5) i += ncomp == 2 ? 8 : 2;  // immediate64
    for (unsigned k = 0; k < o.dim; ++k) {
        const unsigned rep = (o.token >> (22 + 3 * k)) & 7;
        if (rep == 0 || rep == 3) {
            if (i >= n) return 0;
            o.index[k] = tok[i++];
        } else if (rep == 1 || rep == 4) {
            if (i + 1 >= n) return 0;
            o.index[k] = tok[i];
            i += 2;
        }
        if (rep >= 2) {
            o.immediate_indices = false;
            Operand rel;
            i = decode_operand(tok, n, i, rel);
            if (!i) return 0;
        }
    }
    return i <= n ? i : 0;
}

// Finds the operand token to patch (byte offset in the bytecode), or 0.
std::size_t find_view_origin_add(const std::uint8_t* code, std::size_t size) {
    if (size < 32 || std::memcmp(code, "DXBC", 4) != 0) return 0;
    std::uint32_t count = 0;
    std::memcpy(&count, code + 28, 4);
    if (count > 64 || 32 + 4ull * count > size) return 0;
    for (std::uint32_t c = 0; c < count; ++c) {
        std::uint32_t off = 0, csize = 0;
        std::memcpy(&off, code + 32 + 4 * c, 4);
        if (off + 8ull > size) return 0;
        if (std::memcmp(code + off, "SHEX", 4) != 0 && std::memcmp(code + off, "SHDR", 4) != 0) continue;
        std::memcpy(&csize, code + off + 4, 4);
        if (off + 8ull + csize > size || csize < 8) return 0;
        const std::size_t base = off + 8;
        std::vector<std::uint32_t> tok(csize / 4);
        std::memcpy(tok.data(), code + base, tok.size() * 4);
        const std::size_t n = std::min<std::size_t>(tok[1], tok.size());
        if ((tok[0] >> 16) != 0) return 0;  // pixel shaders only
        struct Add {
            Operand input, cb;
        };
        std::vector<Add> plus, minus;
        for (std::size_t i = 2; i < n;) {
            const std::uint32_t t = tok[i];
            const unsigned op = t & 0x7ff;
            const std::size_t len = op == 53 ? (i + 1 < n ? tok[i + 1] : 0) : ((t >> 24) & 0x7f);  // 53: custom data
            if (len == 0) return 0;
            if (op == 0) {  // add
                std::size_t j = i + 1;
                for (bool e = (t >> 31) != 0; e && j < n; ++j) e = (tok[j] >> 31) != 0;
                Operand dst, a, b;
                if ((j = decode_operand(tok.data(), n, j, dst)) && (j = decode_operand(tok.data(), n, j, a)) &&
                    (j = decode_operand(tok.data(), n, j, b)) && a.type == 1 && a.dim == 1 && a.modifier == 0 && b.type == 8 && b.dim == 2 &&
                    b.immediate_indices) {
                    if (b.modifier == 0) plus.push_back({a, b});
                    else if (b.modifier == 1) minus.push_back({a, b});
                }
            }
            i += len;
        }
        std::size_t found = 0, matches = 0;
        for (const Add& p : plus)
            for (const Add& m : minus)
                if (p.input.index[0] == m.input.index[0] && p.cb.index[0] == m.cb.index[0] && p.cb.index[1] == m.cb.index[1]) {
                    ++matches;
                    // A 4-component swizzle is needed to select .zzzz.
                    if ((p.cb.token & 3) == 2 && ((p.cb.token >> 2) & 3) == 1) found = base + 4 * p.cb.pos;
                }
        return matches == 1 ? found : 0;
    }
    return 0;
}

// ------------------------------------------------------------------ the dither patch
// Square Enix's materials take a dither value from a 64x64x64 noise texture, addressed by
// the pixel position and by the view's frame counter times 8 (plus a per-object offset):
//
//     ishl rA.w, cbN[139].z, l(3)                      the frame counter * 8
//     iadd rB.z, rX.w, rA.w                             + the object's offset
//     ftoi rB.xy, v<SV_Position>.xyxx
//     and rB.xyz, rB.xyzx, l(63, 63, 63, 0)
//     mov rB.w, l(0)
//     ld_indexable(texture3d) rC, rB.xyzw, tN           tN declared texture3d
//
// The value thresholds a coverage (hair strands, dithered fades) and dithers the G-buffer's
// normals, so the pattern changes every frame and relies on the temporal anti-aliasing to
// average it out. In a headset that average never settles and hair shimmers. A mask below 8
// in place of 63 removes the frame counter (a multiple of 8) from the lookup: the pattern
// becomes a fixed tile of the same noise, mask + 1 pixels wide.
//
// The same lookup without the per-object offset (the counter written straight into rB.z)
// appears in full-screen passes (shadow projection, ambient occlusion); those are counted,
// and only patched on request (set_dither).

struct DitherScan {
    std::vector<std::size_t> material;  // byte offsets of the mask immediates (x, y, z follow), material form
    std::vector<std::size_t> screen;    // the same, full-screen form
    // The material form's counter instructions (ishl rA, cbN[139].z, l(3)): byte offsets of
    // the opcode token and of the shift immediate. Each instruction once.
    std::vector<std::pair<std::size_t, std::size_t>> counter;
};

DitherScan find_dither(const std::uint8_t* code, std::size_t size) {
    DitherScan out;
    if (size < 32 || std::memcmp(code, "DXBC", 4) != 0) return out;
    std::uint32_t count = 0;
    std::memcpy(&count, code + 28, 4);
    if (count > 64 || 32 + 4ull * count > size) return out;
    for (std::uint32_t c = 0; c < count; ++c) {
        std::uint32_t off = 0, csize = 0;
        std::memcpy(&off, code + 32 + 4 * c, 4);
        if (off + 8ull > size) return out;
        if (std::memcmp(code + off, "SHEX", 4) != 0 && std::memcmp(code + off, "SHDR", 4) != 0) continue;
        std::memcpy(&csize, code + off + 4, 4);
        if (off + 8ull + csize > size || csize < 8) return out;
        const std::size_t base = off + 8;
        std::vector<std::uint32_t> tok(csize / 4);
        std::memcpy(tok.data(), code + base, tok.size() * 4);
        const std::size_t n = std::min<std::size_t>(tok[1], tok.size());
        if ((tok[0] >> 16) != 0) return out;  // pixel shaders only

        struct Instr {
            unsigned op = ~0u;   // ~0u: an instruction of no interest here
            std::size_t at = 0;  // token index of the opcode
            std::vector<Operand> ops;
        };
        std::vector<Instr> ins;  // the instructions after the declarations, in order
        std::set<std::uint32_t> tex3d;
        std::uint32_t position = ~0u;
        for (std::size_t i = 2; i < n;) {
            const std::uint32_t t = tok[i];
            const unsigned op = t & 0x7ff;
            const std::size_t len = op == 53 ? (i + 1 < n ? tok[i + 1] : 0) : ((t >> 24) & 0x7f);  // 53: custom data
            if (len == 0 || i + len > n) return DitherScan{};
            Operand o;
            if (op == 88 && ((t >> 11) & 0x1f) == 5 && decode_operand(tok.data(), n, i + 1, o) && o.type == 7 && o.dim == 1)
                tex3d.insert(o.index[0]);  // dcl_resource_texture3d
            if (op == 100) {  // dcl_input_ps_siv: operand, then the system value
                const std::size_t name_at = decode_operand(tok.data(), n, i + 1, o);
                if (name_at && name_at < i + len && o.type == 1 && o.dim == 1 && tok[name_at] == 1) position = o.index[0];  // POSITION
            }
            if (op < 88) {
                Instr in;
                in.at = i;
                if (op != 53) {  // every instruction but custom data: its operands
                    std::size_t j = i + 1;
                    for (bool e = (t >> 31) != 0; e && j < n; ++j) e = (tok[j] >> 31) != 0;
                    bool ok = true;
                    while (ok && j < i + len) {
                        Operand x;
                        const std::size_t next = decode_operand(tok.data(), n, j, x);
                        if (!next || next > i + len) {
                            ok = false;
                        } else {
                            in.ops.push_back(x);
                            j = next;
                        }
                    }
                    if (ok) in.op = op;
                }
                ins.push_back(std::move(in));
            }
            i += len;
        }
        if (tex3d.empty() || position == ~0u) return out;

        auto is_temp = [](const Operand& o, std::uint32_t reg) { return o.type == 0 && o.dim == 1 && o.modifier == 0 && o.index[0] == reg; };
        auto is_imm = [&tok](const Operand& o, std::initializer_list<std::uint32_t> v) {
            if (o.type != 4 || ((o.token & 3) == 2 ? 4u : 1u) != v.size()) return false;
            std::size_t k = 0;
            for (std::uint32_t x : v)
                if (tok[o.imm + k++] != x) return false;
            return true;
        };
        auto is_counter = [&](const Instr& in) {  // ishl <temp>, cbN[..], l(3)
            return in.op == 41 && in.ops.size() == 3 && in.ops[0].type == 0 && in.ops[1].type == 8 && in.ops[1].dim == 2 && is_imm(in.ops[2], {3});
        };
        for (std::size_t k = 0; k < ins.size(); ++k) {
            const Instr& a = ins[k];
            if (a.op != 1 || a.ops.size() != 3 || a.ops[0].type != 0 || write_mask(a.ops[0]) != 0x7) continue;
            const std::uint32_t r = a.ops[0].index[0];
            if (!is_temp(a.ops[1], r) || !is_imm(a.ops[2], {63, 63, 63, 0})) continue;
            // After it: the 3D texture load from that register.
            bool load = false;
            for (std::size_t m = k + 1; m < ins.size() && m <= k + 3 && !load; ++m) {
                const Instr& l = ins[m];
                load = l.op == 45 && l.ops.size() == 3 && is_temp(l.ops[1], r) && l.ops[2].type == 7 && tex3d.count(l.ops[2].index[0]);
            }
            if (!load) continue;
            // Before it: xy from SV_Position, z from the frame counter.
            bool xy = false;
            int form = 0;  // 1 material, 2 full-screen, -1 something else wrote z
            std::pair<std::size_t, std::size_t> counter{};  // material form: the counter instruction
            for (std::size_t m = k; m-- > 0 && m + 6 >= k;) {
                const Instr& p = ins[m];
                if (p.op == 27 && p.ops.size() == 2 && p.ops[0].type == 0 && p.ops[0].index[0] == r && write_mask(p.ops[0]) == 0x3 &&
                    p.ops[1].type == 1 && p.ops[1].index[0] == position)
                    xy = true;
                if (form != 0) continue;
                if (p.op == ~0u) {  // not decoded: it may write rB.z
                    form = -1;
                    continue;
                }
                if (p.ops.empty() || p.ops[0].type != 0 || p.ops[0].index[0] != r || !(write_mask(p.ops[0]) & 0x4)) continue;
                if (write_mask(p.ops[0]) != 0x4) {
                    form = -1;
                } else if (is_counter(p)) {
                    form = 2;
                } else if (p.op == 30 && p.ops.size() == 3) {
                    // iadd rB.z, x, y: x or y is the counter, from an ishl shortly before.
                    for (std::size_t q = m; q-- > 0 && q + 24 >= m && form == 0;) {
                        const Instr& h = ins[q];
                        if (!is_counter(h)) continue;
                        for (int s = 1; s <= 2; ++s)
                            if (is_temp(p.ops[s], h.ops[0].index[0]) && write_mask(h.ops[0]) == (1u << first_component(p.ops[s]))) form = 1;
                        if (form == 1) counter = {base + 4 * h.at, base + 4 * h.ops[2].imm};
                    }
                    if (form == 0) form = -1;
                } else {
                    form = -1;
                }
            }
            if (!xy || form <= 0) continue;
            (form == 1 ? out.material : out.screen).push_back(base + 4 * a.ops[2].imm);
            if (form == 1 && std::find(out.counter.begin(), out.counter.end(), counter) == out.counter.end()) out.counter.push_back(counter);
        }
        return out;
    }
    return out;
}

// The 3DMigoto hash of a shader (FNV-1, 64 bits, from 0), to compare with fixes made for it.
std::uint64_t migoto_hash(const void* code, std::size_t size) {
    std::uint64_t h = 0;
    for (const std::uint8_t *p = static_cast<const std::uint8_t*>(code), *e = p + size; p < e; ++p) {
        h *= 0x100000001b3ull;
        h ^= *p;
    }
    return h;
}

// A signed copy of the shader with the lookups at `at` masked for a static tile: 2 to 32
// = an x-by-x tile of the first noise slices (the z mask is at most 7, which drops the
// counter, a multiple of 8); 64 = the whole 64x64 tile, the object's offset kept among 8
// slices. Empty if it could not be made.
std::vector<std::uint8_t> dither_copy(const void* code, std::size_t size, const std::vector<std::size_t>& at, int tile) {
    std::vector<std::uint8_t> out(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
    const std::uint32_t xy = static_cast<std::uint32_t>(tile - 1), z = static_cast<std::uint32_t>(std::min(tile, 8) - 1);
    for (std::size_t o : at) {
        if (o + 12 > size) return {};
        std::memcpy(out.data() + o, &xy, 4);
        std::memcpy(out.data() + o + 4, &xy, 4);
        std::memcpy(out.data() + o + 8, &z, 4);
    }
    if (!sign(out)) out.clear();
    return out;
}

// Comparison only: a signed copy whose counter instructions `ishl rA, cbN[139].z, l(3)` become
// `ushr rA, cbN[139].z, l(k)` (same operands, opcode 85 in place of 41): the slice is then
// frame / 2^k + the object's offset, masked by 63 as before. The full noise is kept and the
// pattern changes every 2^k frames instead of every frame. Empty if it could not be made.
std::vector<std::uint8_t> slow_copy(const void* code, std::size_t size, const std::vector<std::pair<std::size_t, std::size_t>>& at, int k) {
    if (at.empty()) return {};
    std::vector<std::uint8_t> out(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
    for (const auto& [op_at, imm_at] : at) {
        if (op_at + 4 > size || imm_at + 4 > size) return {};
        std::uint32_t t = 0;
        std::memcpy(&t, out.data() + op_at, 4);
        if ((t & 0x7ff) != 41) return {};
        t = (t & ~0x7ffu) | 85u;
        std::memcpy(out.data() + op_at, &t, 4);
        const std::uint32_t shift = static_cast<std::uint32_t>(k);
        std::memcpy(out.data() + imm_at, &shift, 4);
    }
    if (!sign(out)) out.clear();
    return out;
}

void STDMETHODCALLTYPE ps_set_shader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* inst, UINT n) {
    if (ps && g_ab_variant.load(std::memory_order_relaxed) >= 0) {
        std::lock_guard lk(g_mutex);
        const auto it = g_variants->find(ps);
        if (it != g_variants->end()) {
            if (ID3D11PixelShader* v = it->second[static_cast<std::size_t>(g_ab_variant.load())]) {
                ps = v;
                ++g_ab_swaps;
            }
        }
    }
    g_set_hook->original<PSSetShaderFn>()(ctx, ps, inst, n);
}

HRESULT STDMETHODCALLTYPE create_pixel_shader(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link,
                                              ID3D11PixelShader** out) {
    // The dither patch: the object is created from a patched copy in place of the game's code.
    std::vector<std::uint8_t> dithered;
    DitherScan ds;
    const int tile = g_dither_size.load(std::memory_order_relaxed);
    try {
        if (code && size >= 32 && out) {  // out == nullptr: the caller only validates the code
            ds = find_dither(static_cast<const std::uint8_t*>(code), size);
            std::vector<std::size_t> at = ds.material;
            if (g_dither_screen.load(std::memory_order_relaxed)) at.insert(at.end(), ds.screen.begin(), ds.screen.end());
            if (tile > 0 && !at.empty()) dithered = dither_copy(code, size, at, tile);
        }
    } catch (...) {
        dithered.clear();
    }
    HRESULT hr = E_FAIL;
    bool patched = false;
    if (!dithered.empty()) {
        hr = g_hook->original<CreatePixelShaderFn>()(dev, dithered.data(), dithered.size(), link, out);
        patched = SUCCEEDED(hr) && out && *out;
        if (!patched) {
            ++g_dither_failed;
            log::warn("shaders: dithered pixel shader {}: patched copy refused (0x{:08X}); the game's shader is used",
                      hash_string(hash_of(code, size)), static_cast<std::uint32_t>(hr));
        }
    }
    if (!patched) hr = g_hook->original<CreatePixelShaderFn>()(dev, code, size, link, out);
    if (SUCCEEDED(hr) && out && *out && code) {
        try {
            Entry e;
            e.hash = hash_of(code, size);
            e.run_hash = patched ? hash_of(dithered.data(), dithered.size()) : e.hash;
            e.size = size;
            std::lock_guard lk(g_mutex);
            if (g_keep_all.load(std::memory_order_relaxed) || g_wanted->count(e.hash)) {
                if (patched) e.code = dithered;
                else e.code.assign(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
                g_kept_bytes += size;
            }
            {
                const std::uint64_t mh = migoto_hash(code, size);
                auto [it, fresh] = g_known->try_emplace(mh);
                if (fresh) {
                    it->second = Known{e.hash, size, static_cast<unsigned>(ds.material.size()), static_cast<unsigned>(ds.screen.size()), patched};
                    if (!ds.material.empty()) ++g_dither_material_seen;
                    if (!ds.screen.empty()) ++g_dither_screen_seen;
                    if (patched) {
                        const std::uint64_t n = ++g_dither_patched;
                        log::debug("shaders: dithered pixel shader {} (3DMigoto {:016x}, {} bytes, {} lookup(s)) patched to a static {}x{} tile",
                                   hash_string(e.hash), mh, size, ds.material.size() + (g_dither_screen.load() ? ds.screen.size() : 0), tile, tile);
                        if (n == 1 || n == 10 || n == 100 || n == 1000)
                            log::info("shaders: dithered materials: {} pixel shader(s) patched so far to a static {}x{} dither tile ([stereo] hair_dither)",
                                      n, tile, tile);
                    }
                } else {
                    it->second.patched = it->second.patched || patched;
                }
            }
            const auto old = g_patched->find(*out);  // an address reused after a release
            if (old != g_patched->end()) {
                old->second.shader->Release();
                g_patched->erase(old);
            }
            // The view-origin copy starts from the bytecode the object was created from.
            const std::uint8_t* src = patched ? dithered.data() : static_cast<const std::uint8_t*>(code);
            if (const std::size_t at = find_view_origin_add(src, size)) {
                ++g_patch_matches;
                std::vector<std::uint8_t> copy(src, src + size);
                std::uint32_t t = 0;
                std::memcpy(&t, copy.data() + at, 4);
                t = (t & ~(0xffu << 4)) | (0xaau << 4);  // .zzzz
                std::memcpy(copy.data() + at, &t, 4);
                ID3D11PixelShader* ps = nullptr;
                const bool signed_ok = sign(copy);
                const HRESULT phr = signed_ok ? g_hook->original<CreatePixelShaderFn>()(dev, copy.data(), copy.size(), link, &ps) : E_FAIL;
                if (SUCCEEDED(phr) && ps) {
                    (*g_patched)[*out] = Patched{ps, e.hash};
                    Entry pe;
                    pe.hash = hash_of(copy.data(), copy.size());
                    pe.run_hash = pe.hash;
                    pe.size = copy.size();
                    pe.code = std::move(copy);
                    (*g_seen)[ps] = std::move(pe);
                } else {
                    ++g_patch_failures;
                }
                if (g_patched_logged->insert(e.hash).second)
                    log::info("shaders: pixel shader {} ({} bytes) adds the view's origin to SV_Position; patched copy {} (0x{:08X})",
                              hash_string(e.hash), size, SUCCEEDED(phr) && ps ? "created" : "FAILED", static_cast<std::uint32_t>(phr));
            }
            const auto oldv = g_variants->find(*out);  // an address reused after a release
            if (oldv != g_variants->end()) {
                for (ID3D11PixelShader* v : oldv->second)
                    if (v) v->Release();
                g_variants->erase(oldv);
            }
            if (g_ab.load(std::memory_order_relaxed) && !ds.material.empty()) {
                std::array<ID3D11PixelShader*, kVariants> v{};
                for (std::size_t k = 0; k < v.size(); ++k) {
                    std::vector<std::uint8_t> c = kTiles[k] > 0   ? dither_copy(code, size, ds.material, kTiles[k])
                                                  : kTiles[k] < 0 ? slow_copy(code, size, ds.counter, -kTiles[k])
                                                                  : std::vector<std::uint8_t>(static_cast<const std::uint8_t*>(code),
                                                                                              static_cast<const std::uint8_t*>(code) + size);
                    if (!c.empty()) g_hook->original<CreatePixelShaderFn>()(dev, c.data(), c.size(), link, &v[k]);
                }
                (*g_variants)[*out] = v;
            }
            (*g_seen)[*out] = std::move(e);  // an address reused after a release replaces the old entry
            ++g_created;
        } catch (...) {
        }
    }
    return hr;
}

using D3DDisassembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

D3DDisassembleFn disassembler() {
    static D3DDisassembleFn fn = [] {
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<D3DDisassembleFn>(GetProcAddress(m, "D3DDisassemble")) : nullptr;
    }();
    return fn;
}

// ------------------------------------------------------------------ DXBC checksum
// MD5's block function over the bytecode after the 20-byte header, with a padding of its own:
// the last block holds the bit count first and (bit count / 4) | 1 last, and the result is
// the raw state without MD5's final step. Checked against the game's own shaders (`shader
// check`) and by the runtime, which refuses a shader whose checksum is wrong.
struct Md5 {
    std::uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;

    static std::uint32_t rol(std::uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

    void block(const std::uint8_t* p) {
        static constexpr std::uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
            0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
            0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
            0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
            0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static constexpr int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        std::uint32_t m[16];
        std::memcpy(m, p, 64);  // little endian
        std::uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                f = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                f = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            const std::uint32_t t = D;
            D = C;
            C = B;
            B = B + rol(A + f + K[i] + m[g], S[i]);
            A = t;
        }
        a += A;
        b += B;
        c += C;
        d += D;
    }
};

Hash checksum(const std::uint8_t* data, std::size_t size) {
    Md5 md;
    const std::uint8_t* p = data + 20;
    const std::size_t n = size - 20;
    const std::uint32_t bits = static_cast<std::uint32_t>(n * 8);
    const std::size_t full = n & ~static_cast<std::size_t>(63);
    for (std::size_t i = 0; i < full; i += 64) md.block(p + i);
    const std::size_t last = n - full;
    std::uint8_t blk[64]{};
    const std::uint32_t tail = (bits >> 2) | 1;
    if (last >= 56) {
        std::memcpy(blk, p + full, last);
        blk[last] = 0x80;
        md.block(blk);
        std::memset(blk, 0, 64);
        std::memcpy(blk, &bits, 4);
        std::memcpy(blk + 60, &tail, 4);
        md.block(blk);
    } else {
        std::memcpy(blk, &bits, 4);
        if (last) std::memcpy(blk + 4, p + full, last);
        blk[4 + last] = 0x80;
        std::memcpy(blk + 60, &tail, 4);
        md.block(blk);
    }
    return Hash{md.a, md.b, md.c, md.d};
}

bool write_file(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

}  // namespace

std::string hash_string(const Hash& h) { return std::format("{:08x}{:08x}{:08x}{:08x}", h[0], h[1], h[2], h[3]); }

bool sign(std::vector<std::uint8_t>& dxbc) {
    if (dxbc.size() < 32 || std::memcmp(dxbc.data(), "DXBC", 4) != 0) return false;
    const Hash h = checksum(dxbc.data(), dxbc.size());
    std::memcpy(dxbc.data() + 4, h.data(), 16);
    return true;
}

bool install(bool keep_all) {
    g_keep_all = keep_all;
    if (g_hook->installed()) return true;
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    ID3D11Device* dev = nullptr;
    const HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr) : E_FAIL;
    if (FAILED(hr) || !dev) {
        log::warn("shaders: probe device failed 0x{:08X}; pixel shader bytecode is not recorded", static_cast<std::uint32_t>(hr));
        return false;
    }
    void* fn = (*reinterpret_cast<void***>(dev))[kCreatePixelShader];
    if (g_ab.load() && !g_set_hook->installed()) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (ctx) {
            const bool set_ok = g_set_hook->create((*reinterpret_cast<void***>(ctx))[kPSSetShader], &ps_set_shader);
            log::info("shaders: dither comparison ([dev] dither_ab = 1): PSSetShader {}", set_ok ? "hooked" : "NOT hooked");
            ctx->Release();
        }
    }
    dev->Release();
    const bool ok = g_hook->create(fn, &create_pixel_shader);
    log::info("shaders: CreatePixelShader {} (bytecode kept: {})", ok ? "hooked" : "NOT hooked",
              keep_all ? "every shader, [dev] keep_shaders = 1" : "the shaders the fixes need");
    dev_commands::add("shader", "shader status | dump <hex pointer> <prefix> | check | list <file> | fnv <hex>: pixel shader bytecode as the game created it",
                      [](std::string_view args) { return command(std::string(args)); });
    return ok;
}

void set_dither(int tile, bool screen) {
    g_dither_size = tile == 2 || tile == 4 || tile == 8 || tile == 16 || tile == 32 || tile == 64 ? tile : 0;
    g_dither_screen = screen;
    log::info("shaders: dithered materials {}", g_dither_size.load() ? std::format("patched to a static {0}x{0} dither tile{1} ([stereo] hair_dither)",
                                                                                     g_dither_size.load(), screen ? ", full-screen passes too" : "")
                                                                     : std::string("as the game: dither pattern changes every frame ([stereo] hair_dither = game)"));
}

int dither_size() { return g_dither_size.load(); }

void set_dither_ab(bool on) { g_ab = on; }

void want(const Hash& hash) {
    std::lock_guard lk(g_mutex);
    g_wanted->insert(hash);
}

bool lookup(ID3D11PixelShader* ps, Hash* hash, std::vector<std::uint8_t>* code) {
    std::lock_guard lk(g_mutex);
    const auto it = g_seen->find(ps);
    if (it == g_seen->end()) return false;
    if (hash) *hash = it->second.hash;
    if (code) *code = it->second.code;
    return true;
}

ID3D11PixelShader* patched_for(ID3D11PixelShader* ps) {
    if (!ps) return nullptr;
    std::lock_guard lk(g_mutex);
    const auto it = g_patched->find(ps);
    if (it == g_patched->end()) return nullptr;
    it->second.shader->AddRef();
    return it->second.shader;
}

std::string dump(ID3D11PixelShader* ps, const std::string& prefix) {
    Hash h{};
    std::vector<std::uint8_t> code;
    if (!lookup(ps, &h, &code)) return std::format("err pixel shader {} was not seen being created", static_cast<void*>(ps));
    if (code.empty()) return std::format("err pixel shader {} ({}) seen, its bytecode was not kept ([dev] keep_shaders = 1)", static_cast<void*>(ps), hash_string(h));
    if (!write_file(prefix + ".dxbc", code.data(), code.size())) return "err could not write " + prefix + ".dxbc";
    std::string asm_note = "no disassembler";
    if (auto dis = disassembler()) {
        ID3DBlob* blob = nullptr;
        if (SUCCEEDED(dis(code.data(), code.size(), 0, nullptr, &blob)) && blob) {
            asm_note = write_file(prefix + ".asm", blob->GetBufferPointer(), blob->GetBufferSize() ? blob->GetBufferSize() - 1 : 0) ? "asm written"
                                                                                                                                       : "asm not written";
            blob->Release();
        } else {
            asm_note = "disassembly failed";
        }
    }
    const Hash c = checksum(code.data(), code.size());
    return std::format("ok {} bytes, checksum {} (recomputed {}{}), {}: {}.dxbc", code.size(), hash_string(h), hash_string(c),
                       c == h ? ", matches" : ", the dither-patched copy", asm_note, prefix);
}

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (a.empty() || a[0] == "status") {
        std::lock_guard lk(g_mutex);
        std::string patched;
        for (const auto& [orig, p] : *g_patched)
            patched += std::format(" {}->{} ({})", static_cast<void*>(orig), static_cast<void*>(p.shader), hash_string(p.hash));
        return std::format("ok pixel shaders created {} (objects known {}), bytecode kept {} bytes, keep all {}, wanted {}, hook {}; "
                           "view-origin patch: matches {} failures {} live{}; dither: tile {} (screen passes {}), distinct shaders {}, "
                           "with the material lookup {}, with the full-screen lookup {}, patched {}, refused {}",
                           g_created.load(), g_seen->size(), g_kept_bytes.load(), g_keep_all.load() ? 1 : 0, g_wanted->size(),
                           g_hook->installed() ? "on" : "off", g_patch_matches.load(), g_patch_failures.load(), patched.empty() ? " none" : patched,
                           g_dither_size.load(), g_dither_screen.load() ? "patched" : "not patched", g_known->size(), g_dither_material_seen.load(),
                           g_dither_screen_seen.load(), g_dither_patched.load(), g_dither_failed.load());
    }
    if (a[0] == "dither") {
        if (a.size() == 2) {
            int v = -2;
            if (a[1] == "off" || a[1] == "none") v = -1;
            else if (a[1] == "game") v = 0;
            else
                for (int k = 1; k < static_cast<int>(kVariants); ++k)
                    if (a[1] == (kTiles[k] > 0 ? std::to_string(kTiles[k]) : std::format("slow{}", -kTiles[k]))) v = k;
            if (v == -2) return "err usage: shader dither off|game|2|4|8|16|32|64|slow2|slow3|slow4|slow5";
            if (!g_ab.load()) return "err needs [dev] dither_ab = 1 at start";
            g_ab_variant = v;
        }
        std::lock_guard lk(g_mutex);
        const int v = g_ab_variant.load();
        return std::format("ok dither comparison {}: bound {}, shaders with variants {}, swaps {}", g_ab.load() ? "on" : "off",
                           v < 0             ? std::string("as created")
                           : v == 0          ? std::string("the game's")
                           : kTiles[v] > 0   ? std::format("static {}", kTiles[v])
                                             : std::format("slow: the game's noise, a new slice every {} frames", 1 << -kTiles[v]),
                           g_variants->size(), g_ab_swaps.load());
    }
    if (a[0] == "list" && a.size() == 2) {
        // Every distinct pixel shader created: 3DMigoto hash, DXBC checksum, size, dither lookups.
        std::ofstream f(a[1]);
        if (!f) return "err could not write " + a[1];
        std::lock_guard lk(g_mutex);
        f << "migoto\tchecksum\tbytes\tdither_material\tdither_screen\tpatched\n";
        for (const auto& [mh, k] : *g_known)
            f << std::format("{:016x}\t{}\t{}\t{}\t{}\t{}\n", mh, hash_string(k.hash), k.size, k.material, k.screen, k.patched ? 1 : 0);
        return std::format("ok {} shaders listed in {}", g_known->size(), a[1]);
    }
    if (a[0] == "dump" && a.size() == 3) {
        const auto p = static_cast<std::uintptr_t>(std::stoull(a[1], nullptr, 16));
        return dump(reinterpret_cast<ID3D11PixelShader*>(p), a[2]);
    }
    if (a[0] == "check") {
        // Recomputes the checksum of every kept shader: all must match the runtime's own.
        std::size_t n = 0, bad = 0;
        std::lock_guard lk(g_mutex);
        for (const auto& [ptr, e] : *g_seen) {
            if (e.code.size() < 32) continue;
            ++n;
            if (checksum(e.code.data(), e.code.size()) != e.run_hash) ++bad;
        }
        return std::format("ok checksums recomputed for {} kept shaders, {} differ", n, bad);
    }
    if (a[0] == "fnv" && a.size() == 2) {
        // The kept shaders with the given 3DMigoto hash (the name shader-replacement tools give a shader).
        const std::uint64_t want = std::stoull(a[1], nullptr, 16);
        std::string r;
        std::size_t n = 0;
        std::lock_guard lk(g_mutex);
        for (const auto& [ptr, e] : *g_seen) {
            if (e.code.empty()) continue;
            ++n;
            if (migoto_hash(e.code.data(), e.code.size()) == want)
                r += std::format(" {} ({}, {} bytes)", static_cast<void*>(ptr), hash_string(e.hash), e.code.size());
        }
        return std::format("ok {} kept shaders hashed; {:016x}:{}", n, want, r.empty() ? " none" : r);
    }
    return "err usage: shader status | dump <hex pointer> <prefix> | check | list <file> | fnv <hex>";
}

}  // namespace ff7vr::engine::shaders
