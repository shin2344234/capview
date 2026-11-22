# CapView

CapView shows a capture card's picture and plays its sound on Windows. With vsync off it puts each frame on screen as soon as the card delivers it, without waiting for the next monitor refresh. It's for playing an Xbox, PS5 or Switch through an HDMI capture card such as a Cam Link 4K.

## Use

Download `CapView.exe` from the [latest release](https://github.com/shin2344234/capview/releases/latest), put it in any folder and run it. It finds the capture card and the card's audio on its own and picks the card's best video mode. Settings go in `capview.ini` next to the exe, and the log goes in `capview.log`.

The exe has no code signature, so Windows SmartScreen may say "Windows protected your PC" the first time you run it. Click More info, then Run anyway.

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

In fullscreen, CapView stays in front of windows on your other monitors. Without that, a window maximized on the next monitor makes Windows send CapView's picture through the desktop compositor, which drops frames. Anything you open on CapView's own screen still comes up in front of it.

## Console settings

On a PS5, turn off "Enable HDCP" in Settings > System > HDMI. With HDCP on, the card gets no picture.

Set any console's audio output to stereo or linear PCM. Capture cards can't pass Dolby or DTS, so those play as silence.

## Frame generation

Frame generation draws extra frames between the captured ones, so a 30 fps game shows at 60 fps or more. Pick a mode in settings:

- CapView's own works on any GPU.
- NVIDIA Smooth Motion needs an RTX 40 or 50 card, or an RTX 30 card with [RTX Encore](#smooth-motion-on-an-rtx-30-card).
- AMD Fluid Motion Frames needs a Radeon RX 6000 or newer, or Radeon 700M or 800M graphics, with the monitor plugged into it. It only adds frames in fullscreen.
- Intel XeSS frame generation runs on Intel Arc and on other GPUs with shader model 6.4. Copy `libxess_fg.dll` and `libxell.dll` from Intel's [XeSS SDK](https://github.com/intel/xess) next to `CapView.exe`. It doubles the frame rate and outputs SDR only.

## Smooth Motion on an RTX 30 card

[RTX Encore](https://github.com/SilyNoMeta/rtx-encore) by SilyNoMeta adds Smooth Motion to RTX 30 cards. It needs one of the exact NVIDIA driver versions on its [Smooth Motion page](https://github.com/SilyNoMeta/rtx-encore/blob/main/docs/SMOOTH-MOTION.md). On an RTX 20 card it only works with DirectX 12, and CapView uses DirectX 11, so it won't help there.

To install it, close CapView and open its folder in File Explorer. Type `powershell` in the address bar, press Enter, and run this in the window that opens:

```powershell
irm https://raw.githubusercontent.com/shin2344234/capview/main/install-rtx-encore.ps1 | iex
```

The [script](install-rtx-encore.ps1) downloads the latest RTX Encore release, about 185 MB, and checks it against the SHA-256 that GitHub lists for it. It then copies `rtx-encore.dll` out of the zip into CapView's folder and names the copy `dxgi.dll`. It won't overwrite a `dxgi.dll` from another program, such as ReShade. Run it again later to update RTX Encore.

To install by hand, download the zip from RTX Encore's [releases](https://github.com/SilyNoMeta/rtx-encore/releases), copy `rtx-encore.dll` from it next to `CapView.exe` and rename the copy `dxgi.dll`. RTX Encore suggests `version.dll` or `winmm.dll` for games, but CapView never loads those names from its own folder.

Then start CapView, press S and pick NVIDIA Smooth Motion as the frame generation mode. CapView turns Smooth Motion on in RTX Encore's settings and offers to restart, because RTX Encore only reads its settings at start. After the restart, CapView's title bar shows `Smooth Motion (RTX Encore)` and the frame rate Smooth Motion puts out. Fullscreen hides the title bar, so check it in a window.

To remove RTX Encore, delete `dxgi.dll`, `rtx-encore.jsonc` and the `rtx-encore-logs` folder from CapView's folder.

## CapView Camera

OBS can't open the capture card while CapView has it open. Tick "CapView Camera for OBS, Discord and browsers" in settings instead, and CapView adds a camera called CapView Camera that shows its picture. The camera installs for your Windows account only, without an administrator prompt.

In OBS, add a Video Capture Device source and pick CapView Camera as the device. The camera has no sound, so add an Audio Input Capture source as well and pick the card's audio device, such as `Digital Input (Cam Link 4K)`. CapView opens audio in shared mode, so OBS can record it while CapView plays it. Other apps that take a webcam, such as Discord or Zoom, list CapView Camera too.

The camera always runs at 1920x1080 and 60 fps in SDR. CapView scales a 4K or 120 fps signal down to fit and converts HDR to SDR. A picture that isn't 16:9 gets black bars. The camera gets the captured frames only, without frame generation or the fps overlay. While CapView has no signal the camera shows black, and after CapView closes, an app that still has the camera open shows a dimmed copy of the last picture.

Apps run as administrator don't list CapView Camera, and that includes OBS when you run it that way. Neither do 32-bit apps or the Windows Camera app.

Untick the setting before you delete CapView, and the camera goes with it. If you've already deleted CapView, this PowerShell command removes the camera:

```powershell
Remove-Item -Recurse "HKCU:\Software\Classes\CLSID\{7BC6709A-957E-427E-A58A-24FAE7A4940C}", "HKCU:\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\{7BC6709A-957E-427E-A58A-24FAE7A4940C}", "$env:LOCALAPPDATA\CapView"
```

Browsers load the camera's DLL when they list cameras and hold on to it until they close. So if the command says access to a `capview-camera` file is denied, the camera itself is already gone. Close the browser, then delete the `CapView` folder in `%LOCALAPPDATA%`.

## Command line

Options on the command line override `capview.ini` for that run. CapView writes an option to `capview.ini` only if you change that setting while it runs.

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

If CapView says "in use by another app", something else has the card open, often OBS, Elgato Studio, Discord or a browser. Close that app, or switch it to [CapView Camera](#capview-camera).

"no picture" means the card gets no signal. Check the console's HDCP setting and the HDMI cable.

If you see "No capture audio found", CapView couldn't match the card's audio device. Press A to pick it.

## Report a bug

Open settings, press "Report a bug", describe what happened and press Send. CapView sends your description to the developer privately, along with a debug report of this PC's hardware and CapView's logs and settings. You don't need an account.

For a problem that comes and goes, tick "Debug mode" in settings first, wait for it to happen again, then report it.

## Build

Install Visual Studio 2022 or the Visual Studio 2022 Build Tools with the "Desktop development with C++" workload, then run `build.bat`. It builds the CapView Camera filter from `src\camera` into `obj\capview-camera.dll` first, then builds `bin\CapView.exe` with that DLL inside it as a resource.

Intel's license doesn't let this repo include the XeSS SDK headers. Before you build, copy `inc/xess_fg/xefg_swapchain.h`, `inc/xess_fg/xefg_swapchain_d3d12.h`, `inc/xell/xell.h` and `inc/xell/xell_d3d12.h` from the [XeSS SDK](https://github.com/intel/xess) into `src\xess`.

`tools\build-new.ps1` builds to `obj\CapView_new.exe` instead and leaves `bin\CapView.exe` alone, so it works while CapView is running. The `tools` folder also has the PowerShell and C# scripts that measure CapView's frame pacing and latency with [PresentMon](https://github.com/GameTechDev/PresentMon).

## License

CapView is under the [MIT License](LICENSE). Intel's XeSS files and RTX Encore come under their own licenses.

CapView Camera uses [softcam](https://github.com/tshino/softcam) by tshino and Microsoft's DirectShow base classes from [Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples). Both are MIT licensed. Their license texts are in [src/camera/LICENSE](src/camera/LICENSE) and [src/camera/baseclasses/LICENSE](src/camera/baseclasses/LICENSE).
