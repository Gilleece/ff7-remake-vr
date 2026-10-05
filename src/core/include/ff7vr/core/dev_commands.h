#pragma once
// Registry for dev pipe commands (\\.\pipe\ff7vr-dev, see src/loader/dev_input.h).
//
// A module registers the first word of its commands with a handler; the pipe
// server passes every line it does not handle itself to dispatch(). Handlers
// run on the pipe thread, one command at a time, and may block for a while
// (for example until a capture is written).
//
//   ff7vr::dev_commands::add("capture", "capture <path prefix>: write both eyes to PNG",
//                            [](std::string_view args) { return std::string("ok"); });
//
// Replies follow the pipe's convention: "ok ..." on success, "err ..." on failure.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::dev_commands {

using Handler = std::function<std::string(std::string_view args)>;

// Registers (or replaces) the handler for `word`. Thread-safe.
void add(std::string_view word, std::string_view help, Handler handler);

// Runs the handler registered for the first word of `line`. Returns false if
// there is none; `reply` is set otherwise (exceptions become "err ...").
bool dispatch(std::string_view line, std::string& reply);

// "word: help" for every registered command, sorted.
std::vector<std::string> help();

}  // namespace ff7vr::dev_commands
