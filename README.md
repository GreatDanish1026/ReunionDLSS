# ReunionDLSS

ReShade add-on for **Life is Strange: Reunion** (Unreal Engine 5.6, DirectX 12) that replaces
the game's TSR upscaler with **NVIDIA DLSS** and adds **DLSS Frame Generation**. The game has
neither. It's separate from [ReunionTweaks](https://github.com/GreatDanish1026/ReunionTweaks)
(graphics settings), and the two can be installed side by side.

Needs an NVIDIA RTX GPU; frame generation needs an RTX 40 or 50 series card and
hardware-accelerated GPU scheduling. ReShade 6.8 with add-on support.

## What it does

- **DLSS** (F9 to switch on and off): DLSS Super Resolution 310.9.16 upscales in place of TSR.
  Motion vectors are computed by the add-on from the depth buffer and the camera (the game's
  own velocity buffer only covers moving objects).
- **Mode**: DLAA, Quality, Balanced, Performance or Ultra Performance set the render
  resolution while DLSS is on; "Game's Resolution Scale" leaves it to the game's menu. Keep the
  game's Secondary Resolution Scale at 100%.
- **Preset**: K (transformer, default), J, L, M or NVIDIA's default.
- **Frame generation**: Off, 2x, 3x, 4x or Dynamic, through NVIDIA Streamline 2.14.16 with
  Reflex. The HUD is kept from warping using the game's own HUD layer. Switching it on from Off
  takes a game restart; the multipliers switch right away.

Settings are in ReShade's Add-ons tab (Settings and Status) and saved in `ReShade.ini` under
`[ReunionDLSS]`.

## Install

1. Install ReShade 6.8 with add-on support for `Iris-Win64-Shipping.exe`
   (`Iris\Binaries\Win64`).
2. Copy `ReunionDLSS.addon64` next to it.
3. Put NVIDIA's `nvngx_dlss.dll` (310.9.16, signed) next to it.
4. For frame generation, put the Streamline 2.14.16 runtime in a `streamline` folder next to
   it: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`,
   `nvngx_dlssg.dll` (all from NVIDIA's Streamline release, signature-checked at start).

Logs: `ReunionDLSS.log` next to the game's exe, Streamline's in `streamline\sl.log`.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools, Windows SDK for `fxc`); `.\build.ps1 -Install`
copies the add-on and the Streamline runtime from `third_party\streamline\bin\x64` (not in the
repository: unzip NVIDIA's release there) into the game folder, with the game closed.

## How it works

`docs/FINDINGS.md` has TSR's passes, the bindings and the View uniform buffer offsets;
`docs/FRAMEGEN.md` the frame generation design. The F10 frame capture (Status tab) writes one
frame of GPU commands to `ReunionDLSS-frame-N.txt`, for finding passes again after a game update.
