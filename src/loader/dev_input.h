#pragma once
// Dev command pipe: \\.\pipe\ff7vr-dev
//
// Lets harness scripts (tools/dev/send-input.ps1) drive the game without
// window focus by injecting a virtual XInput gamepad, and drop markers into
// ff7vr.log. Enabled by [dev] pipe=1 in ff7vr.ini (off by default in code; the
// dev ini in the repo turns it on). One client at a time; text lines in,
// one response line out per command:
//
//   ping                         -> "ok pong xinput_calls=<n>"
//   tap <buttons> [ms]           press, hold ms (default 120), release
//   hold <buttons>               press and keep held (until release)
//   release                      neutral pad
//   stick <L|R> <x> <y> [ms]     deflect a stick (-1..1), hold ms (default 300), recenter
//   trigger <L|R> <0..1> [ms]
//   mark <text>                  write "MARK <text>" into ff7vr.log
//
// <buttons>: names joined with '+': A B X Y UP DOWN LEFT RIGHT START BACK
// LB RB LS RS (LS/RS = stick clicks). Example: "tap DOWN 100".
//
// This is dev scaffolding that can later move to src/dev.

namespace ff7vr::loader::dev_input {

void start();
void stop();

}  // namespace ff7vr::loader::dev_input
