# wasio-player

A bit-perfect audio player for Windows. Plays WAV, DFF and DSF through either
ASIO or WASAPI exclusive mode, with a playlist. Two executables out of one
build: a window and a command line.

![The player window](docs/screenshot.png)

## What it does

**Bit-perfect, or it says so.** No sample rate conversion, no software volume,
no silent fallback. The output format follows the file: a 96 kHz track opens
the device at 96 kHz, a DSD64 file goes out as DSD64. When a device cannot take
what the file needs, playback stops with a message naming the format that was
refused rather than quietly resampling.

**DSD without a detour.** DFF and DSF play as Native DSD where the device
supports it, or as DoP where it does not. DSD64 through DSD512 have been
verified on hardware. DoP512 is refused outright, because it would need a
1,411,200 Hz PCM carrier that no current device provides.

**A playlist that survives real libraries.** Drag files in, reorder them, save
and load M3U8. Paths are UTF-8 throughout, so non-ASCII file names open
correctly. An entry whose file has moved stays in the list marked unavailable
instead of vanishing.

## Build

The Steinberg ASIO SDK is not redistributable, so it is not in this repository.
Download it from Steinberg and point `ASIO_SDK_ROOT` at your copy.

```powershell
cmake -S . -B build -G Ninja -DASIO_SDK_ROOT=<path to ASIOSDK234>
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

`build.bat` does the same and finds `vcvars64.bat` by itself; set `VCVARS` if
it is somewhere unusual. Add `-DCMAKE_BUILD_TYPE=Release` for a release build.
Requires MSVC and C++17; builds with `/W4 /permissive-` and no warnings.

The tests need no audio hardware - they build synthetic files in memory and
drive a fake playback engine.

## The window

`WasioPlayer.exe`, optionally with files to load. Four sections, top to bottom:
the output device, the playlist, the transport, and a status line showing the
format that was actually negotiated along with callback and underrun counts.

Files can be dropped onto the window. Double-clicking a row plays it. The
progress bar can be dragged to seek. Repeat is off, one or all; Shuffle covers
every track once per round.

## The command line

```
WasioPlay --list-devices
WasioPlay [options] <file>...
WasioPlay [options] --playlist <m3u8>
```

| | |
| --- | --- |
| `--backend asio\|wasapi` | which host API carries the stream |
| `--device <name>` | ASIO driver name, or WASAPI endpoint name or id |
| `--dsd native\|dop` | how DSD files reach the device |
| `--repeat off\|one\|all` | |
| `--shuffle` | |

Omit `--device` and the first usable ASIO driver, or the default WASAPI
endpoint, is used.

While playing: `space` pauses and resumes, `left`/`right` seek by ten seconds,
`n` and `p` change track, `q` quits.

```
> WasioPlay --backend asio --playlist album.m3u8
5 track(s) | repeat off
  1. 01 Prelude.wav | PCM | 44100 Hz | 2 ch | 00:04
  ...
1. 01 Prelude.wav | ASIO PCM 44100 Hz, 2 ch, 32-bit, buffer 1024
  [playing] 00:02 / 00:04  callbacks=86 underruns=0
```

### Exit codes

| | |
| --- | --- |
| 0 | played to the end |
| 1 | bad arguments |
| 2 | no such device |
| 3 | the device would not open - including when another program holds it |
| 4 | the file could not be opened or parsed |
| 5 | the device opened but refused this format |
| 6 | DoP512 refused |
| 7 | playback stopped early |

3 and 5 are deliberately separate: one means try again later or close the other
program, the other means this file will never play on this device.

## Playlists

M3U8, UTF-8 without a BOM, `#EXTINF:<seconds>,<title>` followed by the path.
Relative paths resolve against the playlist file's own directory. An entry that
cannot be read keeps the title and duration the playlist claimed and is shown
as unavailable, so a moved file is visible rather than silently dropped.

## Limits

- Not gapless. A track at a different sample rate needs the device reopened,
  which leaves a short gap.
- Pausing keeps the exclusive device open. That is the trade for resuming
  without a click and for an accurate position.
- WAV is integer PCM 16, 24 or 32-bit only - no float, no extensible channel
  mappings. DFF and DSF do not support DST compression or multichannel routing.
- Native DSD over WASAPI is untested; ASIO is the verified path for DSD.

## Licence

MIT - see [LICENSE](LICENSE).
