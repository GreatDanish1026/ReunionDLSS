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

1. Install ReShade 6.8 or later with full add-on support for `Iris-Win64-Shipping.exe`, in
   `Life is Strange Reunion\Iris\Binaries\Win64`.
2. Copy everything from the release zip into that `Win64` folder: `ReunionDLSS.addon64`,
   `nvngx_dlss.dll` and the `streamline` folder (the licence files can go along).
3. Start the game, open ReShade (Home key), Add-ons tab, ReunionDLSS: switch DLSS on (or press
   F9), pick a mode and, if you like, frame generation (takes effect after a restart).
4. In the NVIDIA App, a DLSS model override for the game takes precedence over the Preset.

The zip includes NVIDIA's DLSS 310.9.16 and Streamline 2.14.16 runtime files, all signed by
NVIDIA (the add-on checks Streamline's signature before loading it). Their licences are in
`licenses`, and attribution is in `THIRD_PARTY_NOTICES.txt`.

To uninstall, delete those files. Logs: `ReunionDLSS.log` next to the game's exe, Streamline's
in `streamline\sl.log`.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools, Windows SDK for `fxc`). The NVIDIA runtime isn't in
the repository: unzip `bin\x64` of NVIDIA's Streamline 2.14.16 release into
`third_party\streamline\bin\x64` first. `.\build.ps1 -Install` copies the add-on and the runtime
into the game folder (game closed); `.\build.ps1 -Package` makes the release zip in
`bin\release`.

## How it works

`docs/FINDINGS.md` has TSR's passes, the bindings and the View uniform buffer offsets;
`docs/FRAMEGEN.md` the frame generation design. The F10 frame capture (Status tab) writes one
frame of GPU commands to `ReunionDLSS-frame-N.txt`, for finding passes again after a game update.
