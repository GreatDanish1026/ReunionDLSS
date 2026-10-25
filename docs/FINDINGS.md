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

## Captures

(none yet)
