#include "dev_input.h"

#include "xinput_proxy.h"

#include "ff7vr/core/log.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <charconv>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace ff7vr::loader::dev_input {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\ff7vr-dev";

std::atomic<bool> g_stop{false};
HANDLE g_thread = nullptr;

bool parse_buttons(const std::string& s, WORD& out) {
    out = 0;
    if (s == "none" || s == "NONE") return true;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        for (auto& c : tok) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (tok == "A") out |= XINPUT_GAMEPAD_A;
        else if (tok == "B") out |= XINPUT_GAMEPAD_B;
        else if (tok == "X") out |= XINPUT_GAMEPAD_X;
        else if (tok == "Y") out |= XINPUT_GAMEPAD_Y;
        else if (tok == "UP") out |= XINPUT_GAMEPAD_DPAD_UP;
        else if (tok == "DOWN") out |= XINPUT_GAMEPAD_DPAD_DOWN;
        else if (tok == "LEFT") out |= XINPUT_GAMEPAD_DPAD_LEFT;
        else if (tok == "RIGHT") out |= XINPUT_GAMEPAD_DPAD_RIGHT;
        else if (tok == "START") out |= XINPUT_GAMEPAD_START;
        else if (tok == "BACK") out |= XINPUT_GAMEPAD_BACK;
        else if (tok == "LB") out |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        else if (tok == "RB") out |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        else if (tok == "LS") out |= XINPUT_GAMEPAD_LEFT_THUMB;
        else if (tok == "RS") out |= XINPUT_GAMEPAD_RIGHT_THUMB;
        else return false;
    }
    return true;
}

bool parse_float(const std::string& s, double& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

int parse_ms(const std::vector<std::string>& a, size_t i, int def) {
    if (i >= a.size()) return def;
    int v = def;
    std::from_chars(a[i].data(), a[i].data() + a[i].size(), v);
    if (v < 0) v = 0;
    if (v > 60000) v = 60000;
    return v;
}

SHORT axis(double v) {
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return static_cast<SHORT>(std::lround(v * 32767.0));
}

std::string handle(const std::string& line) {
    std::vector<std::string> a;
    {
        std::stringstream ss(line);
        std::string t;
        while (ss >> t) a.push_back(t);
    }
    if (a.empty()) return "err empty";
    const std::string& cmd = a[0];
    xinput::VirtualPad pad = xinput::virtual_pad();

    if (cmd == "ping") return "ok pong xinput_calls=" + std::to_string(xinput::get_state_calls());
    if (cmd == "mark") {
        std::string text = line.size() > 5 ? line.substr(5) : "";
        log::info("MARK {}", text);
        return "ok";
    }
    if (!xinput::virtual_pad_enabled()) return "err virtual pad disabled ([dev] virtual_pad=0)";
    if (cmd == "release") {
        xinput::set_virtual_pad({});
        return "ok";
    }
    if (cmd == "tap" || cmd == "hold") {
        WORD b = 0;
        if (a.size() < 2 || !parse_buttons(a[1], b)) return "err bad buttons";
        pad.buttons = b;
        xinput::set_virtual_pad(pad);
        log::debug("dev_input: {} {}", cmd, a[1]);
        if (cmd == "hold") return "ok";
        Sleep(static_cast<DWORD>(parse_ms(a, 2, 120)));
        pad.buttons = 0;
        xinput::set_virtual_pad(pad);
        return "ok";
    }
    if (cmd == "stick") {
        double x = 0, y = 0;
        if (a.size() < 4 || !parse_float(a[2], x) || !parse_float(a[3], y)) return "err usage: stick L|R x y [ms]";
        bool left = a[1] == "L" || a[1] == "l";
        (left ? pad.lx : pad.rx) = axis(x);
        (left ? pad.ly : pad.ry) = axis(y);
        xinput::set_virtual_pad(pad);
        Sleep(static_cast<DWORD>(parse_ms(a, 4, 300)));
        (left ? pad.lx : pad.rx) = 0;
        (left ? pad.ly : pad.ry) = 0;
        xinput::set_virtual_pad(pad);
        return "ok";
    }
    if (cmd == "trigger") {
        double v = 0;
        if (a.size() < 3 || !parse_float(a[2], v)) return "err usage: trigger L|R value [ms]";
        BYTE t = static_cast<BYTE>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * 255));
        bool left = a[1] == "L" || a[1] == "l";
        (left ? pad.lt : pad.rt) = t;
        xinput::set_virtual_pad(pad);
        Sleep(static_cast<DWORD>(parse_ms(a, 3, 200)));
        (left ? pad.lt : pad.rt) = 0;
        xinput::set_virtual_pad(pad);
        return "ok";
    }
    return "err unknown command";
}

void serve_client(HANDLE pipe) {
    std::string buf;
    char chunk[512];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &n, nullptr) || n == 0) return;
        buf.append(chunk, n);
        size_t eol;
        while ((eol = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, eol);
            buf.erase(0, eol + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            std::string reply = handle(line) + "\n";
            DWORD w = 0;
            if (!WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &w, nullptr)) return;
        }
    }
}

DWORD WINAPI pipe_thread(void*) {
    log::info("dev_input: listening on \\\\.\\pipe\\ff7vr-dev");
    while (!g_stop) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            log::error("dev_input: CreateNamedPipe failed ({})", GetLastError());
            return 1;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && !g_stop) serve_client(pipe);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        // A client that disconnects mid-"hold" must not leave buttons stuck.
        xinput::VirtualPad p = xinput::virtual_pad();
        if (p.lx || p.ly || p.rx || p.ry || p.lt || p.rt) {
            p.lx = p.ly = p.rx = p.ry = 0;
            p.lt = p.rt = 0;
            xinput::set_virtual_pad(p);
        }
    }
    return 0;
}

}  // namespace

void start() {
    if (g_thread) return;
    g_thread = CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr);
    if (g_thread) SetThreadDescription(g_thread, L"ff7vr dev pipe");
}

void stop() {
    g_stop = true;
    // Unblock ConnectNamedPipe by connecting once.
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

}  // namespace ff7vr::loader::dev_input
