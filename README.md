# notRDP

A Havoc C2 plugin that creates an invisible alternate Windows desktop, streams it to a browser-based viewer, and supports full mouse/keyboard interaction like RDP, but invisible to the target user.

<p align="center">
  <img src="https://github.com/dagowda/notRDP/blob/ed837307a83b8bf05ec4df9f3a76fca7c2422927/notRDP%20logo%402x.png" alt="image_alt">
</p>


## How It Works

1. Creates a hidden desktop via `CreateDesktopW` and launches `explorer.exe` on it
2. Captures the hidden desktop using `PrintWindow` compositing and streams JPEG frames
3. Injects mouse/keyboard input via `PostMessage` directly to hidden desktop windows
4. Renders the stream in a browser viewer with full input capture

## Prerequisites

Cross-compiler: `x86_64-w64-mingw32-gcc` (and optionally `i686-w64-mingw32-gcc` for x86)

### Install on Ubuntu / Debian / Kali

```bash
sudo apt update && sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
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

```bash
make        # x64 only
make both   # x64 + x86
make clean  # remove .o files
```

### Manual (without Make)

```bash
x86_64-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o screenshot.x64.o screenshot.c
x86_64-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o screeninput.x64.o screeninput.c
x86_64-w64-mingw32-gcc -c -Wall -Wno-unused-variable -o notrdp_mgr.x64.o notrdp_mgr.c
```

## Installation

1. Compile the BOFs
2. In the Havoc Client: **Scripts Manager > Load Script > notrdp.py**

## Usage

```
notrdp [port] [quality]   # Start hidden desktop session
notrdp-close              # Close session
```


## notrdpuser

A variant plugin that captures the user's real desktop session instead of creating a hidden one. Includes opsec-friendly keystroke capture via GetAsyncKeyState polling embedded in the screenshot BOF. Packaged as notrdpuser.zip in the same repo extract and load notrdpuser.py the same way.

```
notrdpuser [port] [quality]   # Start user desktop streaming + keystroke capture
notrdpuser-close              # Close session
```
