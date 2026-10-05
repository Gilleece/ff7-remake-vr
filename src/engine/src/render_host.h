#pragma once

#include "ff7vr/engine/stereo_host.h"

namespace ff7vr::engine {

// The StereoHost that connects the engine to the render module's XR session. Only
// available when the build includes the render module (FF7VR_ENGINE_WITH_RENDER).
StereoHost* render_stereo_host();

}  // namespace ff7vr::engine
