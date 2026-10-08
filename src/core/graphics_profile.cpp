#include "ff7vr/core/graphics_profile.h"

#include <cctype>
#include <format>

namespace ff7vr::graphics_profile {
namespace {

// Costs measured at headset resolution are in docs/engine-module.md ("Measured").
// The four level-of-detail lines are the ones the player ini ships with.
constexpr Line kQuality[] = {
    {"foveation", "preset", "quality"},
    {"stereo", "render_scale", "1.0"},
    {"stereo_cvars", "r.StaticMeshLODDistanceScale", "0.5"},
    {"stereo_cvars", "r.SkeletalMeshLODBias", "-1"},
    {"stereo_cvars", "foliage.LODDistanceScale", "2"},
    {"stereo_cvars", "r.ViewDistanceScale", "1.5"},
};

constexpr Line kBalanced[] = {
    {"foveation", "preset", "performance"},
    {"stereo", "render_scale", "1.0"},
    {"stereo_cvars", "r.StaticMeshLODDistanceScale", "0.5"},
    {"stereo_cvars", "r.SkeletalMeshLODBias", "-1"},
    {"stereo_cvars", "foliage.LODDistanceScale", "2"},
    {"stereo_cvars", "r.ViewDistanceScale", "1.5"},
    {"stereo_cvars", "r.Shadow.MaxCSMResolution", "2048"},
    {"stereo_cvars", "r.Shadow.CSM.MaxCascades", "3"},
    {"stereo_cvars", "r.TranslucencyLightingVolumeDim", "32"},
};

// Balanced without the distant detail levels (engine defaults) and without volumetric fog,
// at 90 % of the eye resolution.
constexpr Line kPerformance[] = {
    {"foveation", "preset", "performance"},
    {"stereo", "render_scale", "0.9"},
    {"stereo_cvars", "r.StaticMeshLODDistanceScale", "1"},
    {"stereo_cvars", "r.SkeletalMeshLODBias", "0"},
    {"stereo_cvars", "foliage.LODDistanceScale", "1"},
    {"stereo_cvars", "r.ViewDistanceScale", "1"},
    {"stereo_cvars", "r.Shadow.MaxCSMResolution", "2048"},
    {"stereo_cvars", "r.Shadow.CSM.MaxCascades", "3"},
    {"stereo_cvars", "r.TranslucencyLightingVolumeDim", "32"},
    {"stereo_cvars", "r.VolumetricFog", "0"},
};

std::vector<std::string> g_applied_cvars;

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

std::span<const Line> lines(std::string_view name) {
    const std::string n = lower(name);
    if (n == "quality") return kQuality;
    if (n == "balanced") return kBalanced;
    if (n == "performance") return kPerformance;
    return {};
}

bool known(std::string_view name) {
    const std::string n = lower(name);
    return n == "quality" || n == "balanced" || n == "performance" || n == "custom";
}

const std::vector<std::string>& applied_cvars() { return g_applied_cvars; }

std::vector<std::string> apply(Config& cfg) {
    std::vector<std::string> out;
    const std::string name = lower(cfg.get_string("graphics", "profile", "custom"));
    if (!known(name)) {
        out.push_back(std::format("graphics: [graphics] profile = '{}' is unknown (quality, balanced, performance, custom); "
                                  "nothing applied",
                                  name));
        return out;
    }
    if (name == "custom") {
        out.push_back("graphics: profile custom: nothing applied, every value comes from the ini");
        return out;
    }
    // A chosen profile is the whole point: its values replace what the ini says for those
    // keys (the shipped ini writes every key, so "the ini wins" would leave a profile with
    // nothing to do). profile = custom keeps the ini as written.
    std::string applied, replaced;
    for (const Line& l : lines(name)) {
        const std::string item = std::format("{}.{} = {}", l.section, l.key, l.value);
        const std::string before = cfg.get_string(l.section, l.key, "");
        const bool existed = cfg.set(l.section, l.key, l.value);
        applied += (applied.empty() ? "" : "; ") + item;
        if (std::string_view(l.section) == "stereo_cvars") g_applied_cvars.emplace_back(l.key);
        if (existed && before != l.value)
            replaced += std::format("{}{}.{} (ini: {})", replaced.empty() ? "" : "; ", l.section, l.key, before);
    }
    out.push_back(std::format("graphics: profile {}: applied {}", name, applied.empty() ? "nothing" : applied));
    out.push_back(std::format("graphics: profile {}: replaced the ini's {}", name, replaced.empty() ? "nothing" : replaced));
    return out;
}

}  // namespace ff7vr::graphics_profile
