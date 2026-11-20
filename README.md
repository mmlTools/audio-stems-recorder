# Audio Stems Recorder for OBS Studio

**Record every sound in your stream as its own professional audio layer.**

Audio Stems Recorder is an OBS Studio plugin that captures each selected audio source (and, optionally, each OBS mixer track) into its own sample-aligned file whenever you **record**, **stream**, or start a **manual capture**. Instead of one flat mix you get clean stems that drop straight into any DAW or video editor.

> **Requires OBS Studio 33 or newer** (Windows x64, macOS, Linux). Version 1.x of this plugin targets OBS 31/32.

---

## Installation

### Windows

**Installer (recommended):** run `audio-stems-recorder-<version>-windows-x64-installer.exe`. It installs into the OBS 33+ plugin folder and removes files left by 1.x versions.

**Manual (ZIP):**

1. Close OBS Studio.
2. Copy the `audio-stems-recorder` folder from the ZIP into `C:\ProgramData\obs-studio\plugins\`, so that this file exists:
   ```
   C:\ProgramData\obs-studio\plugins\audio-stems-recorder\audio-stems-recorder.dll
   C:\ProgramData\obs-studio\plugins\audio-stems-recorder\data\...
   ```
3. Start OBS and open **Tools → Audio Stems Recorder...**

OBS 33 changed the plugin folder layout: the DLL now sits directly in the plugin folder, **not** in `bin\64bit\`. The old `bin\64bit` layout loads with a deprecation warning in OBS 33 and stops loading in OBS 34. If you are upgrading from 1.x by hand, delete `C:\ProgramData\obs-studio\plugins\audio-stems-recorder\bin` (and `C:\Program Files\obs-studio\obs-plugins\64bit\audio-stems-recorder.dll`, if you installed it there).

**Portable OBS:** copy the folder into `<portable OBS>\plugins\` instead.

---

## Features

### 🎙️ Isolated, sample-aligned stems
- Any OBS audio source: microphone, game, Discord, music, browser, media, and so on.
- Optional **mixer-track stems** (Track 1–6 full mixes).
- **Pre-fader** (clean, after filters; the default) or **post-fader** (follows the OBS volume slider and mute).
- All stems in a session start at the same instant and have **identical length**. If a source goes silent and stops sending audio (a device unplugged, a media source that ended), the gap is filled with silence so nothing drifts.
- Pausing the recording **pauses the stems too**, so they stay locked to the video.

### ⏺️ Automatic, or manual
- Starts and stops with OBS **Recording** and/or **Streaming**.
- **Manual capture** with no recording or stream running, from the dock or a hotkey.
- An **"Audio Stems" dock** shows live status, elapsed time, and any dropped audio, with Start/Stop, Add Marker, Open Folder and Settings buttons.
- Hotkeys (*Settings → Hotkeys*): "Audio Stems: Start/Stop manual capture" and "Audio Stems: Add marker".

### 🗂️ Organized sessions
```
2026-01-19_20-14-33_RECORDING
 ├─ Mic.wav
 ├─ Game.wav
 ├─ Discord.wav
 ├─ Track_1.wav
 ├─ session.json      (stems, levels, offsets, markers)
 └─ markers.txt       (Audacity label track: scene changes and your markers)
```
Optionally, save stems **next to your OBS recording** in a `<recording name>_stems` folder.

### 🎚️ Formats
- WAV **16-bit**, **24-bit** (default) or **32-bit float**, written natively. Files switch to **RF64** automatically past 4 GB, so even a 12-hour stream records without corruption.
- **FLAC** or **MP3** export through `ffmpeg`, auto-detected from PATH or set in the settings. FLAC and MP3 need ffmpeg installed (for example `winget install ffmpeg`); without it, stems stay WAV.
- Unicode source names and folders are fully supported.

### ✂️ Post-processing (optional, runs in the background after the session)
- Silence trimming. The trimmed offset is recorded in `session.json` so you can realign.
- Loudness normalisation with gated RMS, plus a -0.3 dBFS peak ceiling.

---

## Built for live use

- **The live stream never waits on the plugin.** Audio is copied from OBS into a lock-free ring buffer: no locks, no allocations, no disk I/O on OBS' audio thread. A dedicated writer thread does the disk work.
- **Slow disk ≠ broken stream.** If the disk stalls for longer than the 8-second buffer, the plugin drops stem audio (and reports it in the dock, the log and `session.json`) instead of stalling OBS.
- **Constant memory.** Trimming, normalising and exporting stream through the file in small blocks, so multi-hour stems can't run OBS out of memory. All of it runs on a low-priority background thread after the session ends.
- **Crash-safe.** WAV headers are refreshed every 5 seconds. After a crash, interrupted sessions are repaired automatically the next time OBS starts.
- **Safe with source changes.** Deleting a source mid-session, switching scene collections, or closing OBS while recording are all handled cleanly.

---

## Building

The plugin uses the standard [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) build system. `buildspec.json` pins OBS 33.0.1 and obs-deps 2026-10-23.

**Windows** (Visual Studio 2026 or 2022, PowerShell 7):
```powershell
.github/scripts/Build-Windows.ps1 -Configuration RelWithDebInfo
.github/scripts/Package-Windows.ps1 -Configuration RelWithDebInfo [-Installer]
```
The build installs to `release/<Config>/audio-stems-recorder/` in the OBS 33 layout. Packaging produces `release/audio-stems-recorder-<version>-windows-x64.zip` and, with `-Installer` (needs Inno Setup 6), the matching `-installer.exe`.

`cmake --install build_x64` with no `--prefix` installs straight into `C:\ProgramData\obs-studio\plugins`. Run it from an elevated shell.
