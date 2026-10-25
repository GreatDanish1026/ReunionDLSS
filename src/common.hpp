// Shared between the capture tool (main.cpp), the TSR hook (upscaler.cpp) and the NGX wrapper
// (dlss.cpp).
#pragma once

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

namespace rd {

// ReunionDLSS.log next to the game exe.
void Log(const char* fmt, ...);

// The folder of the game exe, with a trailing backslash.
const std::wstring& ExeDir();

// FNV-1a of a compute pipeline's shader bytecode (0 if unknown): stable across runs.
uint32_t PipeId(reshade::api::pipeline pipe);

// TSR's compute shaders in this game build (docs/FINDINGS.md).
constexpr uint32_t kTsrDilateVelocity = 0xed32101f;
constexpr uint32_t kTsrDecimateHistory = 0xdb4a2625;
constexpr uint32_t kTsrRejectShading = 0xabcfcc34;
constexpr uint32_t kTsrSpatialAntiAliasing = 0x19a32eda;
constexpr uint32_t kTsrUpdateHistory = 0xd235f807;
constexpr uint32_t kTsrResolveHistory = 0x60acc0c7;

// upscaler.cpp
void RegisterUpscaler();
void UnregisterUpscaler();
void DrawUpscalerOverlay();

}  // namespace rd
