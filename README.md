# ReunionDLSS (in development)

ReShade add-on that will replace **Life is Strange: Reunion**'s TSR upscaler with NVIDIA DLSS.
Separate from ReunionTweaks (graphics settings); the two can be installed side by side.

0.0.1 is a frame capture tool only and changes nothing in the game: **F10** (or the button on
its Add-ons tab) writes two frames of GPU work to `ReunionDLSS-frame-N.txt` next to the game
exe, to find TSR's passes and their inputs. See `docs/FINDINGS.md`.

Build: `.\build.ps1` (Visual Studio 2022 C++ tools); `.\build.ps1 -Install` copies it into the
game folder (game closed).
