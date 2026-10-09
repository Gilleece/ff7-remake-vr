#pragma once
// Pixel shader bytecode as the game creates it.
//
// ID3D11Device::CreatePixelShader is hooked from start-up (through a throwaway device of the
// NULL driver, whose device class is the same as the game's), before the engine creates any
// shader; the engine creates its shaders on first use, so every pixel shader of the game goes
// through the hook. For each shader object the hook records the DXBC checksum (bytes 4-19 of
// the bytecode: a fingerprint of the exact shader) and keeps the bytecode of the shaders a
// fix asked for (`want`), or of all of them with `[dev] keep_shaders = 1`.
//
// Dev pipe command `shader ...`:
//   shader status                     shaders seen, bytecode kept
//   shader dump <hex pointer> <prefix> writes <prefix>.dxbc and <prefix>.asm (disassembly) of
//                                     the shader object at that address (as `gpu trace` logs it)

#include <d3d11.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ff7vr::engine::shaders {

using Hash = std::array<std::uint32_t, 4>;  // the DXBC checksum

// Start-up (any thread, before the game creates its device).
bool install(bool keep_all);

// Keep the bytecode of shaders with this checksum from now on.
void want(const Hash& hash);

// The checksum and (if kept) the bytecode of a pixel shader object. False if it was not seen.
bool lookup(ID3D11PixelShader* ps, Hash* hash, std::vector<std::uint8_t>* code);

// The patched copy of a shader that adds the view rectangle's origin to SV_Position where it
// reads its inputs but subtracts it where it derives the screen position (Square Enix's
// screen-space reflections): in the copy the inputs are read at SV_Position, so the shader is
// right for a view drawn at its own rectangle in a double-wide target. Created together with
// the original. Returns a referenced shader (the caller releases it) or nullptr.
ID3D11PixelShader* patched_for(ID3D11PixelShader* ps);

// Writes <prefix>.dxbc and <prefix>.asm. Returns "ok ..." or "err ...".
std::string dump(ID3D11PixelShader* ps, const std::string& prefix);

std::string hash_string(const Hash& h);

// Recomputes the DXBC checksum in place (after a patch of the bytecode). The algorithm is the
// MD5 variant the D3D runtime checks (see shaders.cpp).
bool sign(std::vector<std::uint8_t>& dxbc);

std::string command(const std::string& args);

}  // namespace ff7vr::engine::shaders
