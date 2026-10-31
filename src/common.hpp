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

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rd {

// ReunionDLSS.log next to the game exe.
void Log(const char* fmt, ...);

// The folder of the game exe, with a trailing backslash.
const std::wstring& ExeDir();

// FNV-1a of a compute pipeline's shader bytecode (0 if unknown): stable across runs.
uint32_t PipeId(reshade::api::pipeline pipe);

// TSR's compute shaders in this game build (docs/FINDINGS.md).
constexpr bool kTsrMapped = true;  // (framegen.cpp, shared with DoubleExposureDLSS, checks it)
// Frame generation's inputs go to their frame when they're recorded (true) or when the game submits
// them (false, DoubleExposureDLSS: UE 5.2). UE 5.6 submits on a thread of its own, so by submission
// the next frame's work can come before this frame's Present: inputs then went to the wrong frame
// (jitter around characters while the camera moved, 2026-10-31). Reunion's 1.0.x matched at
// recording and was clean.
constexpr bool kTagAtRecord = true;
constexpr uint32_t kTsrDilateVelocity = 0xed32101f;
constexpr uint32_t kTsrDecimateHistory = 0xdb4a2625;
constexpr uint32_t kTsrRejectShading = 0xabcfcc34;
constexpr uint32_t kTsrSpatialAntiAliasing = 0x19a32eda;
constexpr uint32_t kTsrUpdateHistory = 0xd235f807;
constexpr uint32_t kTsrResolveHistory = 0x60acc0c7;

// upscaler.cpp
void RegisterUpscaler();
void UnregisterUpscaler();
// Overlay: the Settings tab and the Status tab.
void DrawUpscalerSettings();
void DrawUpscalerStatus();

// Overlay helpers (main.cpp), in ReunionTweaks' style.
float ControlWidth();
void Help(const char* text);  // a "(?)" after the last item, with <text> as its tooltip
extern const ImVec4 kWarn;
// Puts the game's pipeline, root signatures, descriptor heaps and bindings back on <cl> after
// the add-on recorded work of its own on it.
void RestoreListState(reshade::api::command_list* cl);

// framegen.cpp: DLSS Frame Generation through Streamline, or FSR Frame Generation (fsrfg.cpp).
void RegisterFrameGen();
void UnregisterFrameGen();
void DrawFrameGenSettings();
void DrawFrameGenStatus();
std::string FrameGenHeadline(bool* warn);
std::string FrameGenDiag();
bool FrameGenIsScreen(unsigned width, unsigned height);
// The View uniform buffer (first 16 KB) of this frame's main view, with View.ClipToPrevClip's offset.
void FrameGenSetCamera(const float* view, int clipToPrevClipOffset);
// This frame's depth (UE's, state depth read + shader resource) and our motion vectors (render
// pixels, previous minus current; state non-pixel shader resource), tagged on <list>.
void FrameGenTag(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, unsigned renderWidth,
                 unsigned renderHeight, float jitterX, float jitterY, bool reset);

}  // namespace rd
