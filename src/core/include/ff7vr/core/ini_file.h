#pragma once
// In-place update of ff7vr.ini: changes the values of some keys and leaves everything else
// (comments, blank lines, order, spacing, line endings) as it was.
//
// For each update: the key's line in its [section] gets the new value in place of the old
// one; an inline comment (" ; ..." or " # ...", the rule Config uses) stays, at the same
// column when the new value fits. A key that is not in its section is added after the
// section's last key line; a section that does not exist is added at the end of the file.
// A key that is in its section only as a line commented out at the start ("; key = value")
// is uncommented in place (with the new value) instead of being added. `comment_out` does the
// opposite: the key's line gets the value and is commented out ("not set"); nothing happens
// when the key has no line. Section and key names are matched without regard to case, like
// Config does.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::ini_file {

struct Update {
    std::string section, key, value;
    bool comment_out = false;
};

struct Report {
    std::vector<std::string> changed;    // "section.key: old -> new"
    std::vector<std::string> added;      // "section.key = value" (key or section was missing)
    std::vector<std::string> unchanged;  // "section.key = value" (already that value)
};

// The text with the updates applied.
std::string apply(std::string_view text, const std::vector<Update>& updates, Report* report = nullptr);

// Reads `path`, applies the updates and writes the file again; the previous file is kept as
// `<path>.bak` (replaced each time). A missing file is created. Returns false with `error`
// set when the file could not be read or written (the original is then left untouched).
bool update_file(const std::filesystem::path& path, const std::vector<Update>& updates, Report* report, std::string* error);

}  // namespace ff7vr::ini_file
