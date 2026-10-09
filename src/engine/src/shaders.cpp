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
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ff7vr::engine::shaders {
namespace {

constexpr std::size_t kCreatePixelShader = 15;  // ID3D11Device vtable slot

struct Entry {
    Hash hash{};
    std::size_t size = 0;
    std::vector<std::uint8_t> code;  // empty unless kept
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
};

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
    if (o.type == 4) i += ncomp == 2 ? 4 : 1;  // immediate32
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

HRESULT STDMETHODCALLTYPE create_pixel_shader(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link,
                                              ID3D11PixelShader** out) {
    const HRESULT hr = g_hook->original<CreatePixelShaderFn>()(dev, code, size, link, out);
    if (SUCCEEDED(hr) && out && *out && code) {
        try {
            Entry e;
            e.hash = hash_of(code, size);
            e.size = size;
            std::lock_guard lk(g_mutex);
            if (g_keep_all.load(std::memory_order_relaxed) || g_wanted->count(e.hash)) {
                e.code.assign(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
                g_kept_bytes += size;
            }
            const auto old = g_patched->find(*out);  // an address reused after a release
            if (old != g_patched->end()) {
                old->second.shader->Release();
                g_patched->erase(old);
            }
            if (const std::size_t at = find_view_origin_add(static_cast<const std::uint8_t*>(code), size)) {
                ++g_patch_matches;
                std::vector<std::uint8_t> copy(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
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
    dev->Release();
    const bool ok = g_hook->create(fn, &create_pixel_shader);
    log::info("shaders: CreatePixelShader {} (bytecode kept: {})", ok ? "hooked" : "NOT hooked",
              keep_all ? "every shader, [dev] keep_shaders = 1" : "the shaders the fixes need");
    dev_commands::add("shader", "shader status | dump <hex pointer> <prefix> | check: pixel shader bytecode as the game created it",
                      [](std::string_view args) { return command(std::string(args)); });
    return ok;
}

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
    return std::format("ok {} bytes, checksum {} (recomputed {}), {}: {}.dxbc", code.size(), hash_string(h), c == h ? "matches" : "DIFFERS",
                       asm_note, prefix);
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
                           "view-origin patch: matches {} failures {} live{}",
                           g_created.load(), g_seen->size(), g_kept_bytes.load(), g_keep_all.load() ? 1 : 0, g_wanted->size(),
                           g_hook->installed() ? "on" : "off", g_patch_matches.load(), g_patch_failures.load(), patched.empty() ? " none" : patched);
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
            if (checksum(e.code.data(), e.code.size()) != e.hash) ++bad;
        }
        return std::format("ok checksums recomputed for {} kept shaders, {} differ", n, bad);
    }
    return "err usage: shader status | dump <hex pointer> <prefix> | check";
}

}  // namespace ff7vr::engine::shaders
