# ReunionDLSS: findings

Game facts (engine, protection, paths, cvars): see ReunionTweaks' `docs/FINDINGS.md`.

## Upscaler today

Only TSR (`r.AntiAliasingMethod=4`), no DLSS/FSR/XeSS/Streamline code in the exe. TSR passes
(shader files named in the exe): ClearPrevTextures, DilateVelocity, DecimateHistory,
RejectShading, SpatialAntiAliasing, UpdateHistory, ResolveHistory, MeasureFlickeringLuma.
Settings: `r.TSR.AsyncCompute=2` (TSR on the async compute queue), `r.TSR.History.ScreenPercentage=200`,
`r.TemporalAASamples=8`, resolution 3840x2160 at 65% (render 2496x1404).
`r.BasePassOutputsVelocity` exists (read-only); there is no `r.Velocity.ForceOutput`, so the
velocity buffer has only moving objects and camera motion must come from depth + matrices
(or TSR's DilateVelocity output).

ReShade 6.8's add-on API has no event for the game's debug markers (`BeginEvent`), so passes
are identified from the capture: dispatch sizes, shader ids (FNV-1a of the compute bytecode,
stable across runs) and the resources they transition and bind.

## Captures (2026-10-25, free roam, 3840x2160 at 65%)

`docs/capture-1..3.txt`: two frames each, about 550 dispatches per frame. Everything runs on the
graphics queue (the compute queue gets empty lists), despite `r.TSR.AsyncCompute=2`.

TSR is six compute dispatches in a row, identical in all three captures and both frames:

| shader id | dispatch | TSR pass (by size and order) |
|---|---|---|
| `ed32101f` | 312x176 (render res / 8) | ClearPrevTextures or DilateVelocity |
| `db4a2625` | 312x176 | DilateVelocity or DecimateHistory |
| `abcfcc34` | 96x54 (26 px tiles: 32 px with 3 px overscan) | RejectShading |
| `19a32eda` | 312x176 | SpatialAntiAliasing |
| `d235f807` | 960x540, writes 7680x4320 rgba16f x4 (history at 200% of output) | UpdateHistory |
| `60acc0c7` | 640x360 (6 px groups), writes 3840x2160 r11g11b10f | ResolveHistory |

The output texture (3840x2160 r11g11b10f) is then read by a full-screen draw (post-processing
chain). Render-resolution textures seen around TSR: depth+stencil 2496x1404 `r32g8x24_typeless`,
several 2496x1404 rgba16f (scene color candidates), rg16unorm and rg16f (velocity and TSR's
dilated velocity candidates).

Resources are bound through descriptor tables (GPU handles; not resolved yet) plus root
constant buffers: param 2 = the pass's own parameters, param 3 = a region every TSR pass and many
others share (`+1540352` in a 4 MB buffer): the View uniform buffer.

## TSR inputs and outputs (capture A, 2026-10-25, 0.0.3 table resolution)

| pass | reads | writes |
|---|---|---|
| DilateVelocity `ed32101f` | t0 scene depth 2496x1404 `r32g8x24_typeless` (D32S8), t1 velocity rgba16unorm | u0 dilated velocity rg16f, u1-u3 closest depth / reprojection data |
| RejectShading `abcfcc34` | t0/t2 scene color and separate translucency (rgba16f), t8 dilated velocity | u4 rgba16f: scene color with translucency (TSR's feed) |
| UpdateHistory `d235f807` | t0 that feed | u0 history 7680x4320 rgba16f |
| ResolveHistory `60acc0c7` | t0 history | u0 output 3840x2160 r11g11b10f, u1 1920x1080 half-res copy |

Tables are padded: entries past the pass's real bindings are stale descriptors.

The intermediates are transient: the dilated velocity's memory is reused right after
SpatialAntiAliasing and the feed's right after UpdateHistory (barriers to `uav` on other
resources at the same addresses). So DLSS copies them while they exist (RejectShading,
UpdateHistory). The depth and the output stay valid at ResolveHistory (depth in state 0xe0,
output in `uav`).

TSR's common parameters (each pass's root CBV param 2, the same first ~300 bytes):
offset 0 render size (2496, 1404), 112 history size (7680, 4320), 128 output (3840, -2160 ...),
**256 jitter in render pixels** (Halton(2,3) - 0.5: frame N `-0.4375, 0.388889`, N+1 `0.0625,
-0.462963`), **272 ScreenVelocityToInputPixelVelocity (1248, -702)**, then its inverse.

DilateVelocity and DecimateHistory are recorded in a different command list from the other four
passes (UE records lists in parallel), so per-frame values are read at ResolveHistory.

## DLSS integration (0.1.0)

ReShade 6.8's D3D12 layer: pipeline = `ID3D12PipelineState*`, layout = `ID3D12RootSignature*`,
resource = `ID3D12Resource*`, descriptor tables are encoded (only ReShade converts them).
`command_list::get_native()` is the unwrapped list, so NGX's own binds bypass ReShade, and
ReShade keeps the game's heaps and root signatures cached; `bind_descriptor_tables` with no
tables makes it set both again. The add-on tracks each list's compute and graphics bindings
(tables, root descriptors, root constants, pipeline) and replays them after DLSS.

DLSS DLL: signed `nvngx_dlss.dll` 310.9.16, copied from Jedi Survivor's DLSS plugin folder
(updated there 2026-10-25) next to the game exe. Feature flags: HDR, low-res MVs, inverted
depth, auto exposure; preset K by default; MV scale = -(TSR's 1248, -702), with flip switches
in the overlay for calibration. Not tested in game yet.

## Motion vectors (0.1.4 samples, 2026-10-25)

Still, then panning right, center strip of each texture:
- DilateVelocity u0 (rg16f): closest depth, not motion. Feeding it to DLSS as motion caused the shimmer.
- DilateVelocity u2 (r32uint x4): reprojection data, no consistent motion during the pan (layer 1 and 3 constant fill).
- Engine velocity t1 (rgba16unorm): **all zero** during the pan. UE writes velocity only for moving
  objects (x > 0 means written; decode xy * (1/(0.499*0.5)) - (32767/65535)/(0.499*0.5)).

Plan (0.1.5): own compute shader in DilateVelocity's list (depth t0 + velocity t1 bound there):
camera motion = screen - (prev.xy/prev.w), prev = (screen, deviceZ, 1) * View.ClipToPrevClip;
object velocity where t1.x > 0; output rg16f in pixels, previous minus current (x * W/2, y * -H/2),
DLSS motion scale 1. Find ClipToPrevClip in the View UB (root CBV param 3) by searching for
TemporalAAJitter = (|jitter.x*2/W|, |jitter.y*2/H|) (jitter from param 2 offset 256); in UE the
order is ClipToPrevClip, ClipToPrevClipWithAA, TemporalAAJitter, so the matrix is 128 bytes before.
Lock the offset after a few matching frames; log both matrices to confirm (identity when still).
Shader compiled with fxc cs_5_1 at build time; ReShade layout: push constants b0 + push descriptors
t0-t1 and u0; Restore() afterwards; views cached and dropped on destroy_resource.
