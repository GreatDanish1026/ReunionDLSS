# Frame generation plan

Goal: DLSS Frame Generation (DLSS-G) through NVIDIA Streamline, in ReunionDLSS, using the
NVIDIA runtime Jedi Survivor ships (`E:\SteamLibrary\steamapps\common\Jedi Survivor\Engine\Plugins\Runtime\Nvidia\Streamline\Binaries\ThirdParty\Win64`):
`sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll` 2.14.16 and
`nvngx_dlssg.dll` 310.9.16 (all NVIDIA-signed). Needs the matching Streamline SDK headers
(`include/sl*.h`, MIT, github.com/NVIDIA-RTX/Streamline) and possibly `sl.pcl.dll` if 2.14
requires the latency-marker plugin separately.

## How it fits

1. **Swapchain.** Streamline's frame generation lives in a proxy swapchain. Streamline is
   initialized in manual-hooking mode (`slInit` with `eUseManualHooking`) when the add-on loads,
   before the game creates its swapchain. The add-on hooks the real DXGI factory's
   `CreateSwapChainForHwnd` and hands back `slUpgradeInterface(swapchain)`. ReShade then wraps
   Streamline's proxy, so the order is: game → ReShade → Streamline (generates frames) → DXGI.
   `slSetD3DDevice` gets the game's device at `init_device`.
2. **Reflex.** DLSS-G requires Reflex low latency on. Per frame: `slGetNewFrameToken`,
   `slReflexSleep` at frame start, and the latency markers (simulation, render submit, present).
   The add-on only sees the render thread, so markers are placed at present and at the first
   command list of the frame.
3. **Inputs per frame (`slSetTag`).** Depth and our motion vectors (render resolution, already
   computed for DLSS; `mvecScale` = 1 / render size), hudless color (the scene before the UI, to
   keep text and HUD from smearing), and the final output size. The hudless copy needs the
   pass where UMG starts drawing: find it with an F10 capture.
4. **Constants (`slSetConstants`).** Camera matrices from the View uniform buffer (root CBV
   param 3): ClipToPrevClip at byte 2176 (known), plus ViewToClip, ClipToView, PrevClipToClip,
   camera position and axes, near/far, FOV. ViewToClip's rows are at byte 448 (seen in the 0.1.5
   log); the rest need a View dump while moving.
5. **Options.** `slDLSSGSetOptions(eOn / eOff)` from the overlay and a hotkey; frame
   generation off in menus and loading screens (reset on camera cuts through `reset`).

## Risks

- ReShade on top of a Streamline proxy is the usual setup in games with native DLSS-G, but
  ReShade's overlay and effects then only see rendered frames.
- The DLSS-G version check: `nvngx_dlssg.dll` must match what `sl.dlss_g.dll` expects; the
  Survivor set already works together in that game.
- UE's own frame pacing and the game's frame limiter may fight Reflex; test with V-Sync off
  and G-Sync on.

## Stopgap

NVIDIA Smooth Motion (NVIDIA App → Graphics → program settings, RTX 40/50, DX12) generates
frames in the driver with no game inputs. Works today; lower quality on UI and fast motion.

## State (0.3.0, 2026-10-25)

Implemented in `src/framegen.cpp`, not yet tested in game. Off by default (`FrameGeneration` in
ReShade.ini); switching it on in the overlay needs a game restart, since Streamline has to be
loaded before the swapchain exists. Streamline logs to `Win64\streamline\sl.log`.
- Streamline loads at ReShade's `init_device` (D3D12), then the system DXGI factory's vtable
  slots 10 (CreateSwapChain) and 15 (CreateSwapChainForHwnd) are hooked; the new swapchain goes
  through `slUpgradeInterface`, and the proxy's Present (8) and Present1 (22) are hooked.
- Per game frame (proxy Present): RenderSubmitEnd, frame generation on only if this frame was
  tagged, PresentStart / PresentEnd, next frame token, `slReflexSleep`, SimulationStart/End,
  RenderSubmitStart.
- Tags at ResolveHistory, after DLSS: depth (eOnlyValidNow, state 0xe0), our motion vectors
  (eValidUntilPresent, non-pixel SR), extent = render size, mvecScale = 1 / render size.
- Constants from the View UB: ViewToClipNoAA at 512 (checked against ViewToClip at 448),
  ClipToPrevClip, ViewToTranslatedWorld at 256 (axes), camera position from RelativeWorldToClip
  (64) and TranslatedWorldToClip (0). Far plane 1e6 (UE's is infinite).
- No hudless or UI buffers yet: the HUD will smear in generated frames.

## Working (0.3.4, 2026-10-25)

Confirmed in game: 2 frames presented per game frame, frame rate doubles. The key was creating
the game's swapchain through Streamline's proxy factory (`slUpgradeInterface` on the factory the
game called): DLSS-G sets itself up in its CreateSwapChain hooks, so upgrading an existing
swapchain (0.3.0-0.3.3) left it on but presenting 0 extra frames.
Known leftovers: a frame now and then has no tags (Streamline then reuses the last ones and logs
an error); no hudless/UI buffers yet; Streamline warns that no backbuffer extent was tagged.
