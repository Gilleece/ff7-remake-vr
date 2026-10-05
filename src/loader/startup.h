#pragma once
// See startup.cpp for the plug-in point where engine hooks and XR
// initialisation are started.

#include "ff7vr/core/startup_context.h"

namespace ff7vr::loader {

// Called once on the bootstrap thread after log, config and crash handler are
// up. Runs before or concurrently with the engine's own startup: do not assume
// GEngine or a D3D device exists yet.
void start_modules(const StartupContext& ctx);

// Called from DLL_PROCESS_DETACH when the process exits normally. Keep it
// trivial: the loader lock is held and other threads are already gone.
void stop_modules();

}  // namespace ff7vr::loader
