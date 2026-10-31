# ReunionDLSS 1.1.0

ReShade add-on for **Life is Strange: Reunion** (Unreal Engine 5.6, DirectX 12) that replaces
the game's TSR upscaler with **NVIDIA DLSS** or **AMD FSR**, and adds **DLSS Frame Generation**
or **FSR Frame Generation**. The game has none of these. It's separate from
[ReunionTweaks](https://github.com/GreatDanish1026/ReunionTweaks) (graphics settings), and the
two can be installed side by side.

| | Graphics card |
|---|---|
| DLSS upscaling | NVIDIA RTX |
| FSR upscaling | any DirectX 12 card: FSR 4 on Radeon RX 6000 and newer, FSR 3.1 elsewhere |
| DLSS Frame Generation | NVIDIA RTX 40 or 50 (3x, 4x and Dynamic: RTX 50), hardware-accelerated GPU scheduling |
| FSR Frame Generation | any DirectX 12 card: FSR 4 on Radeon RX 9000 under Windows 11, FSR 3.1 elsewhere |

ReShade 6.8 with add-on support. Upscaler and frame generation can be mixed (for example DLSS
upscaling with FSR Frame Generation on an RTX 30 card).

**DLSS or FSR?** On an NVIDIA RTX card, use DLSS: in this game FSR 3.1 is visibly noisier around
characters and hair while the camera moves, and FSR Frame Generation carries that noise into the
generated frames. FSR is there for cards DLSS doesn't run on. FSR 4 (Radeon) should do better but
hasn't been tested.

## What it does

- **Upscaler** (F9 to switch on and off): Auto (DLSS on NVIDIA RTX, FSR elsewhere), DLSS or FSR in
  place of TSR. DLSS Super Resolution 310.9.16; FSR through AMD's FSR SDK 2.4.0, which picks FSR 4
  or FSR 3.1 for the card. Motion vectors are computed by the add-on from the depth buffer and the
  camera (the game's own velocity buffer only covers moving objects).
- **Mode**: Native/DLAA, Quality, Balanced, Performance or Ultra Performance set the render
  resolution while the upscaler is on; "Game's Resolution Scale" leaves it to the game's menu.
  Keep the game's Secondary Resolution Scale at 100%.
- **DLSS model**: K (transformer, default), J, L, M or NVIDIA's default. **FSR sharpness**: 0-1.
- **Frame generation**: Off, DLSS 2x / 3x / 4x / Dynamic (NVIDIA Streamline 2.14.16, with Reflex),
  or FSR 2x (AMD's frame generation swapchain; no Reflex, so a little more latency). In HDR the HUD
  is kept from warping using the game's own HUD layer (FSR: **HUD protection**). Switching it on,
  or between DLSS and FSR, takes a game restart; DLSS's multipliers switch right away.

Settings are in ReShade's Add-ons tab (Settings and Status; the Status tab also has FSR testing
tools) and saved in `ReShade.ini` under `[ReunionDLSS]`.

## Install

1. Install ReShade 6.8 or later with full add-on support for `Iris-Win64-Shipping.exe`, in
   `Life is Strange Reunion\Iris\Binaries\Win64`.
2. Copy everything from the release zip into that `Win64` folder: `ReunionDLSS.addon64`,
   `nvngx_dlss.dll`, the three `amd_fidelityfx_*_dx12.dll` files and the `streamline` folder (the
   licence files can go along).
3. Start the game, open ReShade (Home key), Add-ons tab, ReunionDLSS: switch the upscaler on (or
   press F9), pick DLSS or FSR and a mode and, if you like, frame generation (takes effect after a
   restart).
4. In the NVIDIA App, a DLSS model override for the game takes precedence over DLSS model.

The zip includes NVIDIA's DLSS 310.9.16 and Streamline 2.14.16 runtime files, signed by NVIDIA
(the add-on checks Streamline's signature before loading it), and AMD's FSR SDK 2.4.0 runtime
files, signed by AMD. Their licences are in `licenses`, and attribution is in
`THIRD_PARTY_NOTICES.txt`.

Tested at 3840x2160 with HDR on, on an NVIDIA RTX card: DLSS and FSR 3.1 upscaling, DLSS and FSR
Frame Generation. With HDR off, frame generation runs without the HUD protection (the game then
draws its HUD straight onto the screen).

To uninstall, delete those files. Logs: `ReunionDLSS.log` next to the game's exe, Streamline's
in `streamline\sl.log`.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools, Windows SDK for `fxc`). The runtimes aren't in the
repository: unzip `bin\x64` of NVIDIA's Streamline 2.14.16 release into
`third_party\streamline\bin\x64`, and copy `amd_fidelityfx_loader_dx12.dll`,
`amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` from
`Kits\FidelityFX\signedbin` of AMD's FSR SDK 2.4.0 into `third_party\fidelityfx\bin`.
`.\build.ps1 -Install` copies the add-on and the runtimes into the game folder (game closed);
`.\build.ps1 -Package` makes the release zip in `bin\release`.

## How it works

`docs/FINDINGS.md` has TSR's passes, the bindings and the View uniform buffer offsets;
`docs/FRAMEGEN.md` the frame generation design. FSR and the frame generation code are shared with
[DoubleExposureDLSS](https://github.com/GreatDanish1026/DoubleExposureDLSS) (its `docs/FSR.md`
covers FSR). Two things differ in UE 5.6: frame generation's inputs are matched to their frame
when they're recorded (UE 5.6 submits GPU work on a thread of its own, so matching at submission
put them on the wrong frame), and FSR's HUD detection compares the screen with a picture that
differs only where the game's HUD layer has something (UE's own copy from before the HUD also
differs around characters). The F10 frame capture (Status tab) writes one frame of GPU commands to
`ReunionDLSS-frame-N.txt`, for finding passes again after a game update.
