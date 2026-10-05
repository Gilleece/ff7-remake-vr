#include "ff7vr/core/pattern.h"

#include "ff7vr/core/module.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace ff7vr::pattern {
namespace {

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Longest run of fixed bytes: [offset, length).
std::pair<std::size_t, std::size_t> anchor_of(const Pattern& p) {
    std::size_t best_off = 0, best_len = 0, cur_off = 0, cur_len = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p.mask[i]) {
            if (cur_len == 0) cur_off = i;
            ++cur_len;
            if (cur_len > best_len) {
                best_len = cur_len;
                best_off = cur_off;
            }
        } else {
            cur_len = 0;
        }
    }
    return {best_off, best_len};
}

bool matches_at(const std::uint8_t* at, const Pattern& p) {
    for (std::size_t i = 0; i < p.size(); ++i)
        if (p.mask[i] && at[i] != p.bytes[i]) return false;
    return true;
}

bool page_readable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Calls fn(start, size) for every maximal readable, committed run in [start, start+size).
template <class Fn>
void for_each_readable(std::uintptr_t start, std::size_t size, Fn&& fn) {
    std::uintptr_t end = start + size;
    std::uintptr_t run_start = 0;
    std::uintptr_t a = start;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
        std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && page_readable(mbi.Protect);
        if (ok && !run_start) run_start = a;
        if (!ok && run_start) {
            fn(run_start, a - run_start);
            run_start = 0;
        }
        a = std::min(region_end, end);
    }
    if (run_start) fn(run_start, end - run_start);
}

}  // namespace

std::optional<Pattern> Pattern::parse(std::string_view text) {
    Pattern p;
    std::size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
        if (c == '?') {
            ++i;
            if (i < text.size() && text[i] == '?') ++i;
            p.bytes.push_back(0);
            p.mask.push_back(0);
            continue;
        }
        if (i + 1 >= text.size()) return std::nullopt;
        int hi = hexval(text[i]), lo = hexval(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        p.bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        p.mask.push_back(1);
        i += 2;
    }
    if (p.bytes.empty() || std::find(p.mask.begin(), p.mask.end(), 1) == p.mask.end()) return std::nullopt;
    return p;
}

Result scan(std::span<const std::uint8_t> range, const Pattern& p, std::size_t max_matches) {
    Result r;
    if (p.size() == 0 || range.size() < p.size()) return r;
    auto [aoff, alen] = anchor_of(p);
    const std::uint8_t* base = range.data();
    const std::uint8_t* last_start = base + (range.size() - p.size());  // last valid pattern start
    const std::uint8_t* anchor_begin = p.bytes.data() + aoff;

    auto check = [&](const std::uint8_t* anchor_hit) -> bool {
        if (anchor_hit < base + aoff) return true;
        const std::uint8_t* start = anchor_hit - aoff;
        if (start > last_start) return false;  // stop: past the end
        if (matches_at(start, p)) {
            r.matches.push_back(reinterpret_cast<std::uintptr_t>(start));
            if (r.matches.size() >= max_matches) return false;
        }
        return true;
    };

    // Search region for the anchor.
    const std::uint8_t* hay_begin = base + aoff;
    const std::uint8_t* hay_end = last_start + aoff + alen;
    if (alen >= 3) {
        std::boyer_moore_horspool_searcher searcher(anchor_begin, anchor_begin + alen);
        const std::uint8_t* it = hay_begin;
        while (it < hay_end) {
            auto [hit, hit_end] = searcher(it, hay_end);
            if (hit == hay_end) break;
            if (!check(hit)) break;
            it = hit + 1;
        }
    } else {
        // Short anchor: memchr on its first byte.
        const std::uint8_t first = anchor_begin[0];
        const std::uint8_t* it = hay_begin;
        while (it < hay_end) {
            auto hit = static_cast<const std::uint8_t*>(std::memchr(it, first, static_cast<std::size_t>(hay_end - it)));
            if (!hit) break;
            if (hit + alen <= hay_end && std::memcmp(hit, anchor_begin, alen) == 0)
                if (!check(hit)) break;
            it = hit + 1;
        }
    }
    return r;
}

Result scan_module(std::uintptr_t module_base, const Pattern& p, Sections which, std::size_t max_matches) {
    Result total;
    for (const auto& s : module::sections(module_base)) {
        bool want = which == Sections::Executable ? s.executable() : s.readable();
        if (!want) continue;
        for_each_readable(s.start, s.size, [&](std::uintptr_t a, std::size_t n) {
            if (total.matches.size() >= max_matches) return;
            Result r = scan({reinterpret_cast<const std::uint8_t*>(a), n}, p, max_matches - total.matches.size());
            total.matches.insert(total.matches.end(), r.matches.begin(), r.matches.end());
        });
        if (total.matches.size() >= max_matches) break;
    }
    return total;
}

Result scan_module(std::uintptr_t module_base, std::string_view signature, Sections which, std::size_t max_matches) {
    auto p = Pattern::parse(signature);
    if (!p) return {};
    return scan_module(module_base, *p, which, max_matches);
}

Result scan_section(std::uintptr_t module_base, std::string_view name, const Pattern& p, std::size_t max_matches) {
    Result total;
    for (const auto& s : module::sections(module_base)) {
        if (s.name != name) continue;
        for_each_readable(s.start, s.size, [&](std::uintptr_t a, std::size_t n) {
            if (total.matches.size() >= max_matches) return;
            Result r = scan({reinterpret_cast<const std::uint8_t*>(a), n}, p, max_matches - total.matches.size());
            total.matches.insert(total.matches.end(), r.matches.begin(), r.matches.end());
        });
    }
    return total;
}

std::uintptr_t rip(std::uintptr_t insn, std::size_t disp_offset, std::size_t insn_len) {
    std::int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(insn + disp_offset), sizeof(disp));
    return insn + insn_len + static_cast<std::intptr_t>(disp);
}

Result find_string(std::uintptr_t module_base, std::string_view text, bool wide, std::size_t max_matches) {
    Pattern p;
    for (char c : text) {
        p.bytes.push_back(static_cast<std::uint8_t>(c));
        p.mask.push_back(1);
        if (wide) {
            p.bytes.push_back(0);
            p.mask.push_back(1);
        }
    }
    // Include the terminator so "Foo" does not match "FooBar".
    p.bytes.push_back(0);
    p.mask.push_back(1);
    if (wide) {
        p.bytes.push_back(0);
        p.mask.push_back(1);
    }
    return scan_module(module_base, p, Sections::Readable, max_matches);
}

}  // namespace ff7vr::pattern
