# notRDP

# Havoc Hidden Desktop (hdesktop)

A Havoc C2 plugin that creates an invisible alternate Windows desktop, streams it to a browser-based viewer, and supports full mouse/keyboard interaction — like RDP, but invisible to the target user.

## How It Works

1. **hdesktop_mgr** BOF creates a hidden desktop via `CreateDesktopW` and launches `explorer.exe` on it
2. **screenshot** BOF captures the hidden desktop using `PrintWindow` compositing and sends JPEG frames as base64
3. **screeninput** BOF injects mouse clicks and keyboard input into the hidden desktop using `PostMessage`
4. **viewer.html** renders the stream in a browser with full input capture (click, type, scroll, drag)
5. **hdesktop.py** orchestrates everything as a Havoc plugin — manages the BOF pipeline, runs the viewer server, relays input

## Files

| File | Description |
|---|---|
| `hdesktop.py` | Havoc plugin — registers commands, manages streaming loop, relays input |
| `_viewer_server.py` | HTTP server with long-polling for frames and JSON-based input relay |
| `viewer.html` | Browser viewer — renders frames, captures mouse/keyboard, supports popout |
| `screeninput.c` | BOF — injects mouse/keyboard via `PostMessage` (hidden) or `SendInput` (default) |
| `screenshot.c` | BOF — captures desktop via GDI + GDI+ JPEG encoding, outputs base64 |
| `hdesktop_mgr.c` | BOF — creates/closes hidden desktops, launches processes on them |
| `beacon.h` | BOF API header (BeaconDataParse, BeaconPrintf, etc.) |
| `Makefile` | Cross-compilation rules |

## Prerequisites

- **Cross-compiler**: `x86_64-w64-mingw32-gcc` (and optionally `i686-w64-mingw32-gcc` for x86)
- **OS**: Any Linux distro or WSL on Windows

### Install on Ubuntu / Debian / Kali

```bash
sudo apt update
sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
```

### Install on Arch

```bash
sudo pacman -S mingw-w64-gcc
```

### Install on Fedora

```bash
sudo dnf install mingw64-gcc mingw32-gcc
```

## Compiling

### x64 only (most common)

```bash
make
```

This produces:
- `screenshot.x64.o`
- `screeninput.x64.o`
- `hdesktop_mgr.x64.o`

### Both x64 and x86

```bash
make both
```

### Clean build artifacts

```bash
make clean
```

### Manual compilation (without Make)

```bash
# x64
x86_64-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o screenshot.x64.o screenshot.c
x86_64-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o screeninput.x64.o screeninput.c
x86_64-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o hdesktop_mgr.x64.o hdesktop_mgr.c

# x86
i686-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o screenshot.x86.o screenshot.c
i686-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o screeninput.x86.o screeninput.c
i686-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o hdesktop_mgr.x86.o hdesktop_mgr.c
```

## Installation

1. Compile the BOFs (see above)
2. Copy the entire `hdesktop/` directory into your Havoc client's plugin folder
3. In the Havoc Client, load the plugin: **Scripts Manager > Load Script > hdesktop.py**

## Usage

In the Havoc console, with an active demon:

```
# Create a hidden desktop and start streaming
hdesktop

# Open a process on the hidden desktop
hdesktop-run notepad.exe

# Close the hidden desktop and kill all processes on it
hdesktop-close
```

The viewer opens automatically at `http://127.0.0.1:4444`. Click the stream to interact — mouse clicks, keyboard input, scrolling, and dragging all work.

### Viewer Controls

- **Click** anywhere on the stream to interact with the hidden desktop
- **Type** to send keyboard input to the focused window
- **Scroll** for mouse wheel events
- **Drag** for click-and-drag operations
- **Popout** button opens a dedicated fullscreen window for the stream
- **Dropdown** selects which demon to view (if multiple are streaming)

## Architecture

```
Operator Machine                          Target Machine
+-----------------+                       +------------------+
| Browser Viewer  |                       | Beacon (Demon)   |
| (viewer.html)   |                       |                  |
|   click/type    |                       | hdesktop_mgr BOF |
|       |         |                       |  CreateDesktopW  |
|       v         |                       |  explorer.exe    |
| Viewer Server   |    Havoc C2 Channel   |                  |
| (_viewer_server)|<--------------------->| screenshot BOF   |
|   JSON relay    |                       |  PrintWindow     |
|       |         |                       |  GDI+ JPEG       |
|       v         |                       |                  |
| Havoc Plugin    |                       | screeninput BOF  |
| (hdesktop.py)   |                       |  PostMessage     |
|  BOF dispatch   |                       |  WindowFromPoint |
+-----------------+                       +------------------+
```

## License

For authorized security testing and educational use only.
