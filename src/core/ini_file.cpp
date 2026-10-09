#include "ff7vr/core/ini_file.h"

#include <windows.h>

#include <fstream>
#include <sstream>

namespace ff7vr::ini_file {
namespace {

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

struct Line {
    std::string body;  // without the line ending
    std::string eol;   // "\r\n", "\n" or "" (last line without one)
};

enum class LineKind { Blank, Comment, Section, Key, Other };

LineKind classify(std::string_view body, std::string* name) {
    const std::string_view t = trim(body);
    if (t.empty()) return LineKind::Blank;
    if (t[0] == ';' || t[0] == '#') return LineKind::Comment;
    if (t[0] == '[') {
        const size_t close = t.find(']');
        if (name) *name = std::string(trim(t.substr(1, close == std::string_view::npos ? t.size() - 1 : close - 1)));
        return LineKind::Section;
    }
    const size_t eq = t.find('=');
    if (eq == std::string_view::npos) return LineKind::Other;
    if (name) *name = std::string(trim(t.substr(0, eq)));
    return LineKind::Key;
}

// The value of a key line: [vb, ve) is what a new value replaces (from the first non-blank
// character after '=' to the end of the value text, before the padding of an inline
// comment); `old` is the value as Config reads it.
void value_region(const std::string& body, size_t* vb, size_t* ve, std::string* old) {
    const size_t eq = body.find('=');
    size_t b = eq + 1;
    while (b < body.size() && (body[b] == ' ' || body[b] == '\t')) ++b;
    if (b < body.size() && body[b] == '"') {
        const size_t close = body.find('"', b + 1);
        *vb = b;
        *ve = close == std::string::npos ? body.size() : close + 1;
        *old = body.substr(b + 1, (close == std::string::npos ? body.size() : close) - b - 1);
        return;
    }
    size_t comment = std::string::npos;
    if (b < body.size() && (body[b] == ';' || body[b] == '#')) {
        comment = b;  // "key =   ; comment": an empty value
    } else {
        for (size_t i = b + 1; i < body.size(); ++i)
            if ((body[i] == ';' || body[i] == '#') && (body[i - 1] == ' ' || body[i - 1] == '\t')) {
                comment = i;
                break;
            }
    }
    size_t e = comment == std::string::npos ? body.size() : comment;
    while (e > b && (body[e - 1] == ' ' || body[e - 1] == '\t')) --e;
    *vb = b;
    *ve = e;
    *old = body.substr(b, e - b);
}

std::string replace_value(const std::string& body, const std::string& value) {
    size_t vb = 0, ve = 0;
    std::string old;
    value_region(body, &vb, &ve, &old);
    size_t cs = ve;  // the inline comment, after the value's padding
    while (cs < body.size() && (body[cs] == ' ' || body[cs] == '\t')) ++cs;
    const bool hasComment = cs < body.size();
    // An empty value ("key =" or "key =   ; comment") becomes "key = value".
    std::string out = old.empty() ? body.substr(0, body.find('=') + 1) + ' ' : body.substr(0, vb);
    out += value;
    if (hasComment) {
        // The comment keeps its column when the new value fits, otherwise one space before it.
        const size_t pad = out.size() + 1 <= cs ? cs - out.size() : 1;
        out.append(pad, ' ');
        out += body.substr(cs);
    }
    return out;
}

}  // namespace

std::string apply(std::string_view text, const std::vector<Update>& updates, Report* report) {
    std::string bom;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        bom = std::string(text.substr(0, 3));
        text.remove_prefix(3);
    }
    std::vector<Line> lines;
    std::string newline = "\r\n";
    bool newlineSeen = false;
    for (size_t pos = 0; pos < text.size();) {
        const size_t eol = text.find('\n', pos);
        Line l;
        if (eol == std::string_view::npos) {
            l.body = std::string(text.substr(pos));
            pos = text.size();
        } else {
            const bool cr = eol > pos && text[eol - 1] == '\r';
            l.body = std::string(text.substr(pos, eol - pos - (cr ? 1 : 0)));
            l.eol = cr ? "\r\n" : "\n";
            if (!newlineSeen) {
                newline = l.eol;
                newlineSeen = true;
            }
            pos = eol + 1;
        }
        lines.push_back(std::move(l));
    }

    for (const Update& u : updates) {
        const std::string full = (u.section.empty() ? "" : u.section + ".") + u.key;
        std::string name;
        bool inSection = u.section.empty();
        bool found = false;
        size_t lastOfSection = std::string::npos;  // last key line (or the header) of a matching section
        for (size_t i = 0; i < lines.size(); ++i) {
            const LineKind k = classify(lines[i].body, &name);
            if (k == LineKind::Section) {
                inSection = iequal(name, u.section);
                if (inSection) lastOfSection = i;
                continue;
            }
            if (!inSection) continue;
            if (k == LineKind::Key || k == LineKind::Other) lastOfSection = i;
            if (k != LineKind::Key || !iequal(name, u.key)) continue;
            size_t vb = 0, ve = 0;
            std::string old;
            value_region(lines[i].body, &vb, &ve, &old);
            found = true;
            if (old == u.value) {
                if (report) report->unchanged.push_back(full + " = " + u.value);
                continue;
            }
            lines[i].body = replace_value(lines[i].body, u.value);
            if (report) report->changed.push_back(full + ": " + old + " -> " + u.value);
        }
        if (found) continue;
        if (report) report->added.push_back(full + " = " + u.value);
        const Line add{u.key + " = " + u.value, newline};
        if (lastOfSection != std::string::npos || u.section.empty()) {
            const size_t at = lastOfSection == std::string::npos ? 0 : lastOfSection + 1;
            if (at > 0 && lines[at - 1].eol.empty()) lines[at - 1].eol = newline;
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), add);
        } else {
            if (!lines.empty() && lines.back().eol.empty()) lines.back().eol = newline;
            if (!lines.empty() && !trim(lines.back().body).empty()) lines.push_back(Line{"", newline});
            lines.push_back(Line{"[" + u.section + "]", newline});
            lines.push_back(add);
        }
    }

    std::string out = bom;
    for (const Line& l : lines) out += l.body + l.eol;
    return out;
}

bool update_file(const std::filesystem::path& path, const std::vector<Update>& updates, Report* report, std::string* error) {
    std::string text;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (exists) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    const std::string out = apply(text, updates, report);
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f) {
            if (error) *error = "write failed: " + tmp.string();
            return false;
        }
    }
    if (exists) {
        std::filesystem::path bak = path;
        bak += L".bak";
        if (!CopyFileW(path.c_str(), bak.c_str(), FALSE)) {
            if (error) *error = "cannot keep the previous file as " + bak.string();
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = "cannot replace " + path.string() + " (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace ff7vr::ini_file
