#pragma once
// IDA-style signature scanner.
//
//   "48 8B 05 ? ? ? ? 48 85 C0 74 ?? E8"   ('?' and '??' are wildcards)
//
// The scanner picks the longest run of fixed bytes in the signature as an
// anchor, searches for it with Boyer-Moore-Horspool and verifies the full
// signature around each hit. On the 96 MB game image one scan of .text takes
// a few milliseconds (see src/core/tests for the benchmark).
//
//   using namespace ff7vr;
//   auto r = pattern::scan_module(module::main_module().base, "48 8B 1D ? ? ? ? 48 85 DB");
//   if (r.unique()) {
//       auto gengine = pattern::rip(r.first(), 3, 7);   // mov rbx, [rip+disp32]
//   }

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::pattern {

struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;  // 1 = byte must match, 0 = wildcard
    std::size_t size() const { return bytes.size(); }

    // Returns nullopt for malformed text or a signature with no fixed byte.
    static std::optional<Pattern> parse(std::string_view text);
};

struct Result {
    std::vector<std::uintptr_t> matches;  // ascending addresses, capped by max_matches
    bool found() const { return !matches.empty(); }
    bool unique() const { return matches.size() == 1; }
    std::uintptr_t first() const { return matches.empty() ? 0 : matches.front(); }
};

enum class Sections {
    Executable,  // sections with IMAGE_SCN_MEM_EXECUTE (code); the default
    Readable,    // every readable section (use for strings/data)
};

// Scan raw memory. Caller guarantees [data, data+size) is readable.
Result scan(std::span<const std::uint8_t> range, const Pattern& p, std::size_t max_matches = 16);

// Scan a loaded module's sections. Pages that are not committed/readable are
// skipped, so a guarded or decommitted page cannot crash the scan.
Result scan_module(std::uintptr_t module_base, const Pattern& p, Sections which = Sections::Executable,
                   std::size_t max_matches = 16);
Result scan_module(std::uintptr_t module_base, std::string_view signature, Sections which = Sections::Executable,
                   std::size_t max_matches = 16);
// Scan one named section (".text", ".rdata", ...).
Result scan_section(std::uintptr_t module_base, std::string_view section_name, const Pattern& p,
                    std::size_t max_matches = 16);

// Resolve a RIP-relative operand: insn + insn_len + *(int32*)(insn + disp_offset).
// Example: "48 8B 05 xx xx xx xx" (mov rax,[rip+x]) -> rip(addr, 3, 7).
//          "E8 xx xx xx xx" (call) -> rip(addr, 1, 5).
std::uintptr_t rip(std::uintptr_t insn, std::size_t disp_offset, std::size_t insn_len);

// Find a string literal (UTF-8 or UTF-16) in readable sections; returns its address.
Result find_string(std::uintptr_t module_base, std::string_view text, bool wide, std::size_t max_matches = 4);

}  // namespace ff7vr::pattern
