# Pinned third-party dependencies. Everything is fetched into
# third_party/_fetched (FETCHCONTENT_BASE_DIR, set in the root CMakeLists.txt).
# Pin to a full commit hash; note the tag next to it.

include(FetchContent)

# --- MinHook 1.3.4 (BSD-2-Clause) -------------------------------------------
# Chosen over safetyhook because safetyhook's public headers require C++23
# (std::expected) and pull in Zydis; MinHook is plain C, has no dependencies,
# builds under our C++20 / static-CRT rules unchanged, and suspends other
# threads while it patches. VTable hooks are implemented by us in ff7vr_core.
# SOURCE_SUBDIR points at a directory without a CMakeLists.txt so only the
# sources are fetched; we define the target ourselves to control the flags.
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  _no_cmake_
)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
target_compile_options(minhook PRIVATE /W0)
