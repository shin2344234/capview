# CapView

CapView shows a capture card's picture and plays its sound on Windows with as little delay as possible. It's made for playing an Xbox, PS5 or Switch through an HDMI capture card such as a Cam Link 4K.

## Use

Download `CapView.exe` from the [latest release](https://github.com/shin2344234/capview/releases/latest), put it in any folder and run it. It finds the capture card, the best video mode and the card's audio by itself. Settings save to `capview.ini` next to the exe, and the log goes to `capview.log`.

The exe isn't signed, so on its first run Windows SmartScreen may say "Windows protected your PC". Click More info, then Run anyway.

| Key | Action |
| --- | --- |
| S, F2, right-click | Settings |
| F11, Alt+Enter, double-click | Fullscreen |
| Esc | Leave fullscreen |
| M | Mute |
| Up, Down | Volume |
| V | Vsync on or off |
| G | CapView's own frame generation on or off |
| F | Fps overlay |
| D, A | Next video device, next audio input |
| R | Rescan devices |
| F1 | Key list |

In fullscreen, CapView stays in front of windows on your other monitors. A window maximized on the next monitor would otherwise make Windows compose CapView's picture and drop frames. A window you open on CapView's own screen still comes up in front of it.

## Console settings

On a PS5, turn off "Enable HDCP" in Settings > System > HDMI, or there's no picture.

On any console, set audio output to stereo or linear PCM. Capture cards can't pass Dolby or DTS, so those play as silence.

## Smoother picture

Pick a frame generation mode in settings to make 30 fps games look smoother:

- CapView's own works on any GPU.
- NVIDIA Smooth Motion needs an RTX 40 or 50 card, or an RTX 30 card with RTX Encore, below.
- AMD Fluid Motion Frames needs a Radeon RX 6000 or newer, or Radeon 700M or 800M graphics, with the monitor plugged into it. It only adds frames in fullscreen.
- Intel XeSS frame generation runs on Intel Arc and on other GPUs with shader model 6.4. Copy `libxess_fg.dll` and `libxell.dll` from Intel's [XeSS SDK](https://github.com/intel/xess) next to `CapView.exe`. It doubles the frame rate and outputs SDR only.

## Smooth Motion on an RTX 30 card

[RTX Encore](https://github.com/SilyNoMeta/rtx-encore) by SilyNoMeta adds Smooth Motion to RTX 30 cards. It needs one of the exact NVIDIA driver versions on its [Smooth Motion page](https://github.com/SilyNoMeta/rtx-encore/blob/main/docs/SMOOTH-MOTION.md). It doesn't help on an RTX 20 card, where it only supports DirectX 12 and CapView uses DirectX 11.

To install it, close CapView and open its folder in File Explorer. Type `powershell` in the address bar, press Enter, and run this in the window that opens:

```powershell
irm https://raw.githubusercontent.com/shin2344234/capview/main/install-rtx-encore.ps1 | iex
```

The [script](install-rtx-encore.ps1) downloads the latest RTX Encore release, about 185 MB, and checks it against the SHA-256 that GitHub lists for it. Then it copies `rtx-encore.dll` out of the zip and puts it next to `CapView.exe` as `dxgi.dll`. Run it again to update RTX Encore. It won't replace a `dxgi.dll` that belongs to another program, such as ReShade.

By hand, download the zip from RTX Encore's [releases](https://github.com/SilyNoMeta/rtx-encore/releases), copy `rtx-encore.dll` from it next to `CapView.exe` and rename the copy `dxgi.dll`. RTX Encore suggests `version.dll` or `winmm.dll` for games, but CapView never loads those names from its own folder.

Then start CapView, press S and pick NVIDIA Smooth Motion as the frame generation mode. CapView turns Smooth Motion on in RTX Encore's settings and offers to restart, since RTX Encore reads them at start. After the restart, CapView's title bar in a window shows `Smooth Motion (RTX Encore)` with the frame rate it puts out.

To remove RTX Encore, delete `dxgi.dll`, `rtx-encore.jsonc` and the `rtx-encore-logs` folder from CapView's folder.

## OBS, Discord and browsers

CapView holds the capture card open, so OBS can't open the card at the same time. Instead, tick "CapView Camera for OBS, Discord and browsers" in settings. CapView then adds a camera called CapView Camera that carries its picture. It installs for your Windows account only and doesn't ask for administrator rights.

In OBS, add a Video Capture Device source and pick CapView Camera as the device. The camera has no sound, so also add an Audio Input Capture source with the card's audio device, such as Digital Input (Cam Link 4K). Discord, Zoom and browsers list CapView Camera with your other cameras.

The camera always runs at 1920x1080 and 60 fps in SDR. CapView scales a 4K or 120 fps signal down to that, converts HDR to SDR, and adds black bars to a picture of another shape. The camera gets the captured picture without frame generation or the fps overlay. It shows black while CapView has no signal. When CapView closes, an app that has the camera open keeps a dimmed copy of the last picture.

OBS run as administrator doesn't list CapView Camera, and neither do other apps run as administrator, 32-bit apps or the Windows Camera app.

Untick the setting before you delete CapView, so the camera goes away with it. If CapView is already gone, this removes the camera:

```powershell
Remove-Item -Recurse "HKCU:\Software\Classes\CLSID\{7BC6709A-957E-427E-A58A-24FAE7A4940C}", "HKCU:\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\{7BC6709A-957E-427E-A58A-24FAE7A4940C}", "$env:LOCALAPPDATA\CapView"
```

Browsers load the camera's DLL when they list cameras and keep it until they close. If the command says access to a `capview-camera` file is denied, the camera is already removed, and the `CapView` folder in `%LOCALAPPDATA%` can go once the browser is closed.

## Command line

Options on the command line override `capview.ini` for that run. CapView only saves one to `capview.ini` if you change that setting while it runs.

| Option | Effect |
| --- | --- |
| `--video <name>` | Opens the video device whose name contains this text |
| `--audio <name>` | Opens the audio input whose name contains this text, or none with `--audio none` |
| `--output <name>` | Plays the sound on the output whose name contains this text |
| `--mode 1920x1080@60` | Asks the card for this video mode |
| `--framegen <mode>` | Frame generation: `off`, `capview`, `smoothmotion`, `afmf` or `xess` |
| `--multiplier <2-4>` | How many frames CapView's own frame generation shows per captured frame |
| `--hdr off` | Shows an HDR source in SDR instead of HDR10 |
| `--fullscreen` | Starts in fullscreen |
| `--exclusive` | Uses exclusive fullscreen, which takes the display while CapView has focus |
| `--vsync` | Starts with vsync on |
| `--volume <0-200>` | Volume in percent |
| `--debug` | Turns on debug mode |
| `--list` | Prints the video devices with their modes, the audio inputs and the audio outputs, then quits |

## Notifications

A Windows notification can drop the picture to about 30 fps while it's on screen. To stop that, go to Settings > System > Notifications and tick "When using an app in full-screen mode" under "Turn on do not disturb automatically".

## Problems

"in use by another app" means OBS, Elgato Studio, Discord or a browser has the card open. Close it, or point that app at CapView Camera instead, as described under OBS, Discord and browsers.

"no picture" means the card gets no signal. Check the console's HDCP setting and the HDMI cable.

"No capture audio found" means CapView couldn't match the card's audio. Press A to pick it.

## Report a bug

Open settings, press "Report a bug", describe what happened and press Send. CapView sends your description with a debug report (this PC's hardware, CapView's logs and settings) privately to the developer. You don't need an account.

If the problem comes and goes, tick "Debug mode" in settings first, make it happen, then report it.

## Build

Run `build.bat` with Visual Studio 2022 or its Build Tools (C++ workload). The exe lands in `bin\CapView.exe`. The script first builds the CapView Camera filter from `src\camera` into `obj\capview-camera.dll`, and the exe carries that DLL inside it.

Intel's license doesn't allow this repo to carry the XeSS SDK headers. Before building, copy `inc/xess_fg/xefg_swapchain.h`, `inc/xess_fg/xefg_swapchain_d3d12.h`, `inc/xell/xell.h` and `inc/xell/xell_d3d12.h` from the [XeSS SDK](https://github.com/intel/xess) into `src\xess`.

To build while CapView is running, run `tools\build-new.ps1`. It writes `obj\CapView_new.exe` and leaves `bin\CapView.exe` alone.

The `tools` folder also has the PowerShell and C# scripts used to measure CapView's frame pacing and latency with [PresentMon](https://github.com/GameTechDev/PresentMon).

## License

CapView is under the [MIT License](LICENSE). The XeSS SDK headers, the XeSS DLLs and RTX Encore keep their own licenses.

CapView Camera is built on [softcam](https://github.com/tshino/softcam) by tshino and on Microsoft's DirectShow base classes from [Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples). Both are under the MIT License, in [src/camera/LICENSE](src/camera/LICENSE) and [src/camera/baseclasses/LICENSE](src/camera/baseclasses/LICENSE).
