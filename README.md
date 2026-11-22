# CapView

CapView shows a capture card's picture and plays its sound on Windows with as little delay as possible. It's made for playing an Xbox, PS5 or Switch through an HDMI capture card such as a Cam Link 4K.

## Use

Put `CapView.exe` in any folder and run it. It finds the capture card, the best video mode and the card's audio by itself. Settings save to `capview.ini` next to the exe.

| Key | Action |
| --- | --- |
| S, F2, right-click | Settings |
| F11, Alt+Enter, double-click | Fullscreen |
| Esc | Leave fullscreen |
| M | Mute |
| Up, Down | Volume |
| V | Vsync on or off |
| G | Frame generation on or off |
| L | Lossless Scaling on or off |
| F | Fps overlay |
| D, A | Next video device, next audio input |
| R | Rescan devices |
| F1 | Key list |

## Console settings

On a PS5, turn off "Enable HDCP" in Settings > System > HDMI, or there's no picture.

On any console, set audio output to stereo or linear PCM. Capture cards can't pass Dolby or DTS, so those play as silence.

## Smoother picture

Pick a frame generation mode in settings to make 30 fps games look smoother:

- CapView's own works on any GPU.
- NVIDIA Smooth Motion needs an RTX 40 or 50 card. On an RTX 20 or 30 card, copy [RTX Encore](https://github.com/SilyNoMeta/rtx-encore)'s `rtx-encore.dll` next to `CapView.exe` as `dxgi.dll`.
- AMD Fluid Motion Frames needs a Radeon RX 6000 or newer, or Radeon 700M or 800M graphics, with the monitor plugged into it. It only adds frames in fullscreen.
- Lossless Scaling needs the Steam app. Pick it in settings or press L, and CapView starts Lossless Scaling, goes fullscreen and turns scaling on with no clicks in Lossless Scaling. It sets Lossless Scaling to LSFG 3 at 2x, or at the multiplier picked in CapView's settings. The fps overlay shows the frame rate Lossless Scaling puts on screen.
- Intel XeSS frame generation runs on Intel Arc and on other GPUs with shader model 6.4. Copy `libxess_fg.dll` and `libxell.dll` from Intel's [XeSS SDK](https://github.com/intel/xess) next to `CapView.exe`. It doubles the frame rate and outputs SDR only.

## Notifications

A Windows notification can drop the picture to about 30 fps while it's on screen. To stop that, go to Settings > System > Notifications and tick "When using an app in full-screen mode" under "Turn on do not disturb automatically".

## Problems

"in use by another app" means OBS, Elgato Studio, Discord or a browser has the card open. Close it.

"no picture" means the card gets no signal. Check the console's HDCP setting and the HDMI cable.

"No capture audio found" means CapView couldn't match the card's audio. Press A to pick it.

## Report a bug

Open settings, press "Report a bug", describe what happened and press Send. CapView sends your description with a debug report (this PC's hardware, CapView's logs and settings) privately to the developer. You don't need an account.

If the problem comes and goes, tick "Debug mode" in settings first, make it happen, then report it.

## Build

Run `build.bat` with Visual Studio 2022 or its Build Tools (C++ workload). The exe lands in `bin\CapView.exe`.
