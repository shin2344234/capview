# CapView

A Windows window that shows a capture card's picture and plays its sound with as little delay as the hardware allows. It's built for consoles (Xbox, PS5, Switch, Switch 2) going through an HDMI capture card.

## Build

Run `build.bat`. It needs Visual Studio 2022 or its Build Tools with the C++ workload. The exe lands in `bin\CapView.exe` and needs no other files.

## Use

Start `CapView.exe`. It finds the capture card, the card's best video mode and the card's own audio input by itself. The title bar shows what it picked, the frame rate it is actually drawing and the audio delay.

| Key | Action |
| --- | --- |
| S, F2, right-click | Settings window |
| F11, Alt+Enter, double-click | Fullscreen (borderless) |
| Esc | Leave fullscreen |
| V | Vsync on or off. Off is the default and the lowest latency, but can tear |
| M | Mute |
| Up, Down | Volume, 0 to 200% |
| D | Next video device, then back to auto |
| A | Next audio input, then none, then back to auto |
| G | CapView frame generation on or off |
| L | Start Lossless Scaling and send its scaling hotkey |
| F | Fps overlay on or off |
| R | Rescan and reopen everything |
| F1 | Key list |

The settings window picks the video device, video mode, audio input, audio output (any playback device, or the Windows default), volume, mute, vsync, fullscreen at start, exclusive fullscreen, frame generation, HDR output and the fps overlay. Changes apply at once and go into `capview.ini` next to the exe, which CapView reads at every start.

Command line options override the ini for that run:

```
--video "Cam Link"        pick a video device by part of its name
--audio "Cam Link"        pick an audio input by part of its name, or "none"
--output "Speakers"       pick an audio output by part of its name
--mode 1920x1080@60       ask for a mode (falls back to auto if the card doesn't offer it)
--framegen capview        off, capview, smoothmotion or lossless
--hdr auto                auto (HDR10 output for HDR sources) or off
--multiplier 2            2, 3 or 4 (CapView frame generation only)
--fullscreen  --vsync  --volume 80
--exclusive               exclusive fullscreen, see below
--debug                   debug mode for this run, see Bug reports
--list                    print devices, modes, outputs and what auto would pick, then exit
```

Command line values apply to that run only. CapView doesn't write them into `capview.ini` unless you change the same setting in the settings window.

Each run writes `capview.log` next to the exe. Other programs can read it while CapView runs. At start CapView renames the previous run's log to `capview.prev.log`, so restarting after a problem keeps the log of the run that had it.

## Notifications

A Windows notification on screen makes the desktop compositor draw CapView, and with Smooth Motion the picture drops to about 30 fps until the notification goes away. Turn on Do Not Disturb for full-screen apps to stop it. In Settings > System > Notifications, under "Turn on do not disturb automatically", tick "When using an app in full-screen mode". Notifications then wait while CapView is fullscreen.

CapView says this in a message box the first time it starts. Its "Open notification settings" button opens that Settings page. CapView then writes `dndTipShown=1` to `capview.ini`. Delete that line to see the message again.

## Exclusive fullscreen

Fullscreen is a borderless window by default. Tick "Exclusive" next to Fullscreen in settings, or start with `--exclusive` (`exclusive=1` in `capview.ini`), and CapView takes the display through `IDXGISwapChain::SetFullscreenState` instead. The display keeps its desktop resolution and refresh rate.

DXGI won't do this for a swap chain made with the frame latency waitable object, so CapView makes a swap chain without it on the way in and a waitable one again on the way out. Each switch costs a hitch of about 100 to 160 ms. Exclusive mode only lasts while CapView has focus. Click another window and CapView goes back to borderless until you click it again.

What it buys is the shell's notification state. While CapView is exclusive, Windows reports it as a Direct3D fullscreen app (`SHQueryUserNotificationState` returns 3) and holds toasts back, the same as Do Not Disturb's "when playing a game" rule. It doesn't stop the stutter from a window that draws on top anyway. A topmost window over CapView makes DXGI drop out of exclusive mode, the desktop compositor takes over, and with Smooth Motion the screen shows about 30 of 60 frames for as long as that window is up, exactly as in borderless. Measured with PresentMon on 21 November 2026.

## Fps overlay

Press F, or tick "Show fps" in settings, for an overlay in the top left corner. The title bar shows the content rate and the output rate while the overlay is off, as in `1920x1080 YUY2 60 Hz (30 fps, 60 out)`. The mode's rate there is the HDMI signal's refresh rate.

Content is the game's own frame rate. A console sends 60 Hz over HDMI whatever the game renders and repeats the last frame when the game runs slower, so the card always delivers 60 frames a second. CapView compares each captured frame with the one before on a grid of luma samples, every second row and every second pixel, and counts only the frames that changed. The same line shows the signal rate and how many repeated frames per second it carried. A paused game or a still menu reads 0 fps with "picture not changing". Anything that moves on screen counts, so a menu with an animated cursor over a paused game reads at the cursor's rate. The log gets a line once a minute with the content rate and how many frames came within a few samples of the cutoff.

Output is the number of frames per second that actually reached the screen, taken from the swap chain's own frame statistics (`IDXGISwapChain::GetFrameStatistics`). Next to it are the average frame time and the worst frame time in the last half second, so a stutter shows up even when the average looks fine. Drawn is how many frames CapView submitted. With vsync on, frames drawn faster than the display refreshes never reach the screen, and the overlay says how many. Display shows the monitor's refresh rate and whether vsync is on. Color shows the source's transfer (SDR, HDR10 or HLG), what CapView outputs and whether Windows HDR is on. The last line shows the frame generation mode.

The counter sees CapView's own frames, including the ones CapView frame generation adds. Frames that NVIDIA Smooth Motion or Lossless Scaling add happen after CapView hands its frame over, so they don't show up in it. Use the NVIDIA overlay or Lossless Scaling's own counter for those.

## HDR

When the card reports an HDR10 (PQ) or HLG signal and Windows HDR is on for the monitor CapView is on, CapView switches its output to 10-bit HDR10 with BT.2020 color and sends HDR10 metadata with it. The title bar then says HDR10. If either goes away, it switches back to SDR. CapView checks the Windows HDR state every two seconds, so dragging the window to another monitor or toggling HDR in Windows takes effect without a restart. For HDR sources it picks a 10-bit format such as P010 over an 8-bit one, since 8 bits would band. The overlay text follows the SDR brightness slider in Windows so it doesn't glare. Set "HDR output for HDR sources" off in settings, or use `--hdr off`, to always output SDR.

The capture card has to take HDR for any of this to apply. A first-generation Cam Link 4K is SDR only, so the console sends it SDR and CapView shows SDR. Cards with HDR passthrough or HDR capture (Elgato 4K X and 4K S, AVerMedia Live Gamer 4K and others) report HDR10 when the console sends it.

Dolby Vision can't work through a capture card. A console only sends Dolby Vision to a display whose EDID advertises it, and no capture card does, so the console falls back to HDR10. Even if a card were to accept it, Dolby Vision over HDMI arrives as a tunneled RGB signal with Dolby's own metadata, and only a licensed Dolby Vision decoder in a TV can turn that back into a picture. Set the console to HDR10 instead.

## Frame generation

DLSS Frame Generation and FSR 3 Frame Generation can't run here. Both are built into games and need the game's own motion vectors and depth buffer, and a capture card delivers finished pictures with neither. CapView has three other options.

CapView interpolation works on any GPU. It runs on the game's own frames, not on the 60 Hz signal. Each capture is compared with the last content frame on the GPU, and the repeats a console sends when a game runs below 60 fps are skipped. A compute shader estimates motion between the last two content frames (block matching over a four-level image pyramid, then a median filter on the motion field), and a pixel shader draws 1 to 3 frames between them, spaced evenly across the game's frame interval. The interval is the average of the last four gaps, in whole signal periods, so an occasional dropped frame doesn't throw the spacing off. A 30 fps game at 2x gives 60 distinct frames a second on screen, and a 60 fps game at 2x gives 120. To draw a frame between A and B, CapView has to hold B back, so each real frame arrives about (M-1)/M of a game frame late: 17 ms at 2x for a 30 fps game, 8 ms for a 60 fps one. Above 60 output frames a second it only helps on a display faster than 60 Hz, and works best with vsync off. Where the two motion-compensated frames disagree by more than about 8% in any color channel, at a pixel or 2 motion-field pixels to each side, the motion estimate there is wrong. CapView then cross-fades the two frames in place instead of following it. Headlights and other small bright lights that move apart from what surrounds them get a soft ghost at both positions instead of a smear. Fast motion, thin text and HUD elements over moving backgrounds can still blur. A still picture gets no in-between frames.

Predict ahead, next to the multiplier in settings (`predict=1` in capview.ini), removes that wait. Each real frame goes on screen the moment it arrives, and the extra frames push the newest frame forward along the last motion estimate instead of filling the gap behind it. Where the newest and previous frames disagree after that shift, CapView shows the newest frame unmoved. With prediction on at 2x, the log's `new frame shown after` time dropped from 17.7 ms to about 1 ms for a 30 fps game. The cost is overshoot. Anything that stops, turns or appears suddenly is drawn one extra frame past where it ends up. When the picture goes still, CapView presents the real frame again so the screen doesn't stay on a predicted one. Prediction is off by default.

NVIDIA Smooth Motion is the driver's own frame generation. Picking it writes a "CapView" profile through NVAPI with Smooth Motion on for CapView.exe, which needs administrator rights once, so Windows asks. The driver reads the profile when CapView starts, so CapView offers to restart itself. NVIDIA supports it on RTX 40 and 50 cards. Picking another mode turns the profile setting off again.

Smooth Motion interpolates between the frames CapView presents, so in this mode CapView presents only the game's own frames and skips the repeats HDMI carries. A 30 fps game reaches the driver as 30 distinct frames a second and comes out as 60. Presenting all 60 signal frames would give it pairs of identical pictures and add nothing. RTX Encore wraps CapView's swap chain and doesn't implement `GetFrameStatistics` (it returns `E_NOTIMPL`), so CapView reads the same counts through `IDXGISwapChainMedia::GetFrameStatisticsMedia`. Through RTX Encore that count includes the frames Smooth Motion adds. The overlay then shows the measured rate on screen next to what CapView drew, and the title shows it without a tilde, as in `Smooth Motion (RTX Encore) 60 fps`. When the count doesn't include them, CapView shows twice its own output as an estimate, as in `~60 fps`. RTX Encore's own fps counter always uses that estimate for Smooth Motion. With RTX Encore loaded, CapView reads `rtx-encore-logs\rtx-encore-status.json` every two seconds and counts Smooth Motion as running only while that file says it's active for this CapView process. Without RTX Encore, only RTX 40 and 50 cards count as running it, and on others the title says "not available on this GPU".

CapView runs one frame generator at a time. Picking a mode in CapView keeps RTX Encore in step, but Smooth Motion can still be switched on from RTX Encore's own menu in another mode, so CapView works around it. In CapView frame generation mode its in-between frames pause while Smooth Motion runs, and the title says "frame gen paused for Smooth Motion". In Lossless Scaling mode CapView doesn't send the Lossless Scaling hotkey, and the title says "Lossless Scaling held for Smooth Motion". CapView also won't send that hotkey over its own frame generation. In every mode, CapView draws each new picture once while Smooth Motion is on, so the driver interpolates game frames, not repeated signal frames. CapView reads `smoothMotionEnabled` from rtx-encore.jsonc at start, so this holds from the first frame, before RTX Encore writes its status file. A still picture is presented again every half second so the overlay keeps updating.

On an RTX 30 card the driver doesn't know the Smooth Motion setting, so the profile write fails and CapView says so. Driver 621.24 on an RTX 3090 Ti rejects it this way. [RTX Encore](https://github.com/SilyNoMeta/rtx-encore) by SilyNoMeta adds Smooth Motion to RTX 20 and 30 cards from inside the program. CapView doesn't ship it, since the project has no license that allows that. Download it from its releases page and copy `rtx-encore.dll` next to `CapView.exe` as `dxgi.dll`. CapView links `dxgi.dll` itself, so Windows loads that name from the exe folder. `version.dll` and `winmm.dll`, the names RTX Encore suggests for most games, never load from there for CapView. When CapView finds RTX Encore loaded, with its `rtx-encore.jsonc` or `rtx-encore-logs` beside it, the log says `RTX Encore loaded as dxgi.dll` and picking NVIDIA Smooth Motion leaves the driver profile alone. CapView switches RTX Encore instead. Picking Smooth Motion sets `"smoothMotionEnabled": true` and `"smoothMotionApi": "d3d11"` in `rtx-encore.jsonc`, and picking any other mode sets `smoothMotionEnabled` back to `false`. Only those values change. Comments, line endings and every other setting in the file stay as they were. RTX Encore reads the file when CapView starts, so CapView offers to restart. CapView also checks the file at startup. If the saved mode or `--framegen` doesn't match what RTX Encore loaded, CapView fixes the file and offers the same restart, and a restart keeps the `--framegen` choice. If the file can't be written, CapView says so and points to RTX Encore's own menu (Insert, the Frame rate tab). Smooth Motion has no multiplier. It's always 2x.

Lossless Scaling (the Steam app) does its own frame generation on any window. Pick LSFG in Lossless Scaling once. Then press L in CapView or use the button in settings. CapView starts Lossless Scaling through Steam if it isn't running, brings the CapView window to the front and sends Lossless Scaling's scaling hotkey. CapView reads the hotkey from Lossless Scaling's own `Settings.xml`. If that fails it uses Ctrl+Alt+S, or whatever you type into the hotkey box. Press L again to stop scaling. With the Lossless mode saved, CapView does this by itself at every start.

## What it detects

Video. Any Media Foundation capture device. It scores devices by name so capture cards (Elgato, AVerMedia, Magewell, Razer Ripsaw, generic HDMI/UVC dongles and others) win over webcams and virtual cameras. Among the card's modes it takes the highest resolution, then the highest frame rate, then the format that is cheapest to get onto the GPU. For an HDR source it takes a 10-bit format over an 8-bit one. A Cam Link 4K flashed with the open firmware from enjoy-digital (USB ID 1209:0001) is the exception, and there YUY2 always wins. That firmware's NV12 and M420 modes only carry the HDMI picture when the card's onboard frame-buffer memory works and the console sends exactly that resolution. Otherwise they stream a test pattern of colored bars. So Auto opens 1080p60 YUY2 on that card, and a 4K mode picked in settings still opens as asked. NV12, YUY2, P010, RGB32, AYUV and Y210 go straight to the GPU. Anything else (I420, UYVY, v210, MJPEG, H.264, HEVC) is decoded or converted by Media Foundation with hardware decoding where the GPU has it. Color matrix (BT.601, BT.709, BT.2020), limited or full range, HDR10 and HLG transfer, pixel aspect ratio and interlacing come from the format the card reports.

Signal changes. When the console switches resolution or frame rate, or the cable is pulled, the stream stops or reports a new format. CapView reopens the device and picks the new best mode. A capture card plugged in later is picked up within about three seconds. If frames stop for a second with no format change, CapView reopens the card too, since a Cam Link 4K on the open firmware sometimes stops sending video mid-stream and starts again only when reopened. Each of those reopens leaves a `Picture back ... ms after the last frame` line in the log. On a Cam Link 4K that's about 3.3 s, 2 s of it spent closing the device.

Audio. The card's audio input is matched to the video device by Windows container ID first and by name second, so it never grabs a microphone by mistake. The converter reads 8, 16, 24 and 32-bit integer and 32 and 64-bit float PCM, any sample rate and up to 32 channels, and writes whatever the output device uses. Surround from the console is downmixed to stereo (or mapped to your speaker layout) with standard coefficients. A small adaptive resampler keeps the card's clock and the output device's clock in step, so the delay stays flat instead of creeping up or clicking. With "Windows default output" picked, audio follows the default device when it changes. With a specific output picked, CapView keeps using it. If it is unplugged, sound moves to the default until it comes back.

## Latency

Video uses Media Foundation's low latency mode, a flip-model swap chain with a frame latency of one, and presents each frame the moment it arrives. With vsync off and tearing support (the title shows when a GPU lacks it), there is no wait for the next monitor refresh. CapView keeps its render target and video processor views from one frame to the next and only rebuilds them after a resize or an HDR switch. The fps overlay is redrawn only when its text changes. The coarse levels of the frame generation pyramid are built only for frames that start a new motion estimate. The frame generation shaders take about 0.9 s to compile, so they compile on a separate thread from launch, and the picture doesn't freeze while they do. On the test machine these changes moved CapView's own GPU work per frame by 0.2 to 0.4 ms, and the capture queue already stayed under 1 ms.

Audio uses the shortest shared-mode engine period the driver allows (WASAPI `IAudioClient3`) on both the card and the output, keeps about two periods queued on output, and holds the card-side buffer near one period. Each output underrun raises that target by 2 ms, and it never comes back down. On a Cam Link 4K, whose audio period is 10 ms, the target settles at 17.7 ms within about 40 seconds and the delay averages about 29 ms inside Windows. A test that lowered the target again after 30 seconds without an underrun got a new underrun within 3 seconds every time, and starting from a smaller target only reached 17.7 ms through more underruns. The card's period is the floor. Bluetooth headphones add their own delay on top.

Every 30 seconds the log (`capview.log`) records a `Video timing` line, with CapView's GPU work per frame and how long a new frame took to reach the screen, and an `Audio timing` line with the average delay, the underrun count and the queue target. `CAPVIEW_TIMING_SECS` in the environment changes the interval. `CAPVIEW_STALL_HOLD_SECS` keeps a stalled video stream open that many seconds longer before CapView reopens the card, so the card can be read while it's still stalled. Reading a Cam Link 4K needs WinUSB on its interface 2, and `tools\camlink_driver.ps1 -Mode WinUSB` switches it with Windows' own WinUSB driver after a UAC prompt. The card has no sound in Windows until `-Mode Audio` puts the audio driver back.

## Console settings

PS5: turn off "Enable HDCP" under Settings, System, HDMI, or there is no picture. Also turn off HDR or set it to "On when supported" if the card can't take HDR.

Xbox Series and Xbox One: no HDCP for games by default. Set the TV resolution to what the card supports (Cam Link 4K takes up to 4K30 or 1080p60).

Switch and Switch 2: no setup needed from the dock. Switch 2 can output 4K and HDR; set it to 1080p if the card can't take that.

Audio on all of them: set the console's audio output to stereo or linear PCM. Capture cards can't decode Dolby Digital, DTS or Atmos bitstream, so those come through as silence or noise.

## Troubleshooting

"in use by another app" means Elgato Studio, OBS, Discord or a browser has the card open. Most cards allow only one app at a time.

"no picture (no signal, or HDCP is on)" means the card is streaming nothing. Check the console's HDCP setting and the HDMI cable.

"No capture audio found" means no audio input matched the card. Press A to choose one by hand.

## Bug reports

Open settings (S or F2) and press "Save debug report" under Troubleshooting. CapView writes `CapView-debug-<date>-<time>.zip` to the Desktop, falling back to the exe's folder, and selects it in Explorer. Attach that zip to the report.

The zip holds `report.txt`, `capview.log`, `capview.prev.log`, `capview.ini`, the three newest crash dumps, and, when RTX Encore is loaded, `rtx-encore.jsonc` with the newest logs and crash logs from `rtx-encore-logs`. `report.txt` lists the Windows build, CPU, memory, power plan, each GPU with its driver version, each display with its name, mode, scale and HDR state, hardware GPU scheduling, Game Mode, the DirectX and compatibility settings for CapView.exe, the notification state, running programs known to touch capture or frame pacing (OBS, Discord, RTSS, NVIDIA overlay, Lossless Scaling and others), the capture and audio devices, every setting, and the live frame rates and counters. The text files show the Windows user folder as `%USERPROFILE%`. Crash dumps hold process memory and are not cleaned.

For a problem that comes and goes, tick "Debug mode (detailed log)" first, make the problem happen, then save the report. Debug mode stays on across restarts (`debug=1` in `capview.ini`) until you untick it, and the title bar shows "debug" while it's on. It adds to the log:

- the same system report at start and whenever debug mode is turned on
- a line every second with content, signal, drawn and shown fps, the longest gap between shown frames, audio delay and CapView's CPU use
- each change of foreground window, with its program, position and how much of CapView's monitor it covers
- each change of the Windows notification state
- window messages for activation, resizing, display, DPI, power and device changes
- `Video timing` and `Audio timing` lines every 5 seconds instead of every 30

A crash always writes `capview-crash-<date>-<time>.dmp` next to the exe and a `CRASH` line to the log. In debug mode the dump also holds the memory the stacks point at, about 2 MB instead of a few hundred KB. `CAPVIEW_CRASH_TEST=1` in the environment makes CapView crash right after start, to check that dumps get written. `build.bat` keeps the matching symbols in `obj\CapView.pdb`. Keep a copy of the PDB for each build you release, or its dumps can't be read.
