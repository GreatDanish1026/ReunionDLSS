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
