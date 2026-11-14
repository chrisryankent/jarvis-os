#!/usr/bin/env python3
"""
JARVIS brain: the language model that reasons and acts for JARVIS OS.

Runs on the host next to the voice bridge. A local model (default: the
Llama 3.2 3B Instruct in tools/models/brain/model.gguf) is served by
llama.cpp's llama-server; the user's requests arrive from the guest as
ASK: lines, and the model answers or calls tools that JARVIS OS executes
(kernel/ai_tools.c). Destructive tools wait for the user's Allow click in
JARVIS OS, so the model can never delete, kill or shut down by itself.

Started by tools/voice_bridge.py (`make run-voice`); every tool runs in the
real JARVIS OS.
"""
from __future__ import annotations

import http.client
import itertools
import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LLAMA_SERVER = ROOT / "vendor" / "llama.cpp" / "build" / "bin" / "llama-server"
DEFAULT_MODEL = ROOT / "models" / "brain" / "model.gguf"
PORT = 8180

def _tool(name: str, description: str, props: dict | None = None, required: list[str] | None = None) -> dict:
    return {"type": "function", "function": {
        "name": name, "description": description,
        "parameters": {"type": "object", "properties": props or {}, "required": required or []}}}


S = {"type": "string"}
I = {"type": "integer"}
TOOLS = [
    _tool("open_app", "Open an app (Files, Terminal, Settings, Calculator, Tasks, Notepad, Apps, Security, Disks, Device Manager, Photos, Clock, Run).", {"name": S}, ["name"]),
    _tool("close_app", "Close an open app window.", {"name": S}, ["name"]),
    _tool("list_apps", "List the apps on this PC and which are open."),
    _tool("list_files", "List the files and folders in a folder.", {"path": S}, ["path"]),
    _tool("find_files", "Find files by part of their name.", {"name": S}, ["name"]),
    _tool("read_file", "Read a text file (first 3000 characters).", {"path": S}, ["path"]),
    _tool("write_file", "Create or replace a text file with the given text.", {"path": S, "text": S}, ["path", "text"]),
    _tool("create_folder", "Create a folder.", {"path": S}, ["path"]),
    _tool("delete", "Move a file or folder to the Recycle Bin.", {"path": S}, ["path"]),
    _tool("move", "Move or rename a file or folder.", {"from": S, "to": S}, ["from", "to"]),
    _tool("copy", "Copy a file or folder.", {"from": S, "to": S}, ["from", "to"]),
    _tool("open_file", "Open a file in Notepad, or a folder in Files.", {"path": S}, ["path"]),
    _tool("system_status", "Time, date, memory, disk, network."),
    _tool("list_processes", "List running processes: PID, state, priority, parent, memory."),
    _tool("kill_process", "End a process by PID.", {"pid": I}, ["pid"]),
    _tool("suspend_process", "Pause a process by PID.", {"pid": I}, ["pid"]),
    _tool("resume_process", "Resume a paused process by PID.", {"pid": I}, ["pid"]),
    _tool("set_priority", "Change a process's priority by PID.", {"pid": I, "level": {"type": "string", "enum": ["low", "normal", "high"]}}, ["pid", "level"]),
    _tool("list_devices", "List the hardware devices, their drivers and status."),
    _tool("device_details", "Details of one device: maker, hardware ID, driver, interrupts, status.", {"name": S}, ["name"]),
    _tool("set_device_enabled", "Disable or enable a device (network adapter, sound card, USB device).", {"name": S, "enabled": {"type": "boolean"}}, ["name", "enabled"]),
    _tool("list_drives", "List the drives with free space."),
    _tool("list_disks", "Disk management: every disk, its file system, space, data read and written, errors."),
    _tool("check_disk", "Check the JARVIS disk for errors in the background; repair fixes them; surface reads every block.", {"repair": {"type": "boolean"}, "surface": {"type": "boolean"}}),
    _tool("disk_check_status", "Progress or result of the disk check."),
    _tool("eject_drive", "Safely eject a USB drive so it can be unplugged.", {"name": S}),
    _tool("network_info", "Network connection details."),
    _tool("set_theme", "Change the look: dark, light or high_contrast.", {"mode": {"type": "string", "enum": ["dark", "light", "high_contrast"]}}, ["mode"]),
    _tool("change_setting", "Change any system setting. Names: brightness (10-100, up, down), night_light (on/off), night_light_strength, "
          "resolution (e.g. 1920x1080), screen_off_min, theme (dark/light/high_contrast), accent (blue/red/green/gold/purple/orange), "
          "wallpaper (bloom, aurora, mountains, dunes, night, waves, solid, or a picture path), wallpaper_fit, wallpaper_color, "
          "transparency, taskbar_align (center/left), desktop_icons, clock_24h, clock_seconds, volume (0-100, up, down), mute, "
          "voice_volume, mouse_speed (slow/normal/fast/fastest), scroll_lines, dnd, lock_after_min, airplane, device_name, autoplay.",
          {"name": S, "value": S}, ["name", "value"]),
    _tool("get_settings", "All system settings and their current values."),
    _tool("run_command", "Run a Terminal command and get its output (ls, cat, grep, find, cp, mv, mkdir, df, ps, ifconfig, ping, uname, free, dmesg, tree, sha256sum ...; also 'timer 5m', 'alarm 07:30 Wake up', 'alarm list', 'stopwatch start'). Use for anything the other tools cannot do.", {"command": S}, ["command"]),
    _tool("disk_action", "Disk management: list, info, create a volume, format, label, extend, shrink, delete, init. "
          "disk is a number from list (0 = system disk) or 'usb'. format, delete and init erase data: only with confirm true after the user agreed.",
          {"action": {"type": "string", "enum": ["list", "info", "create", "format", "label", "extend", "shrink", "delete", "init"]},
           "disk": S, "volume": S, "size_mb": I, "label": S, "fs": {"type": "string", "enum": ["fat32", "fat16", "auto"]},
           "confirm": {"type": "boolean"}}, ["action"]),
    _tool("device_action", "Device Manager: info, enable, disable, restart, uninstall (driver), scan (for hardware changes), events, driver.",
          {"action": {"type": "string", "enum": ["info", "enable", "disable", "restart", "uninstall", "scan", "events", "driver"]}, "name": S}, ["action"]),
    _tool("screenshot", "Take a screenshot (saved in Pictures/Screenshots)."),
    _tool("notify", "Show a notification.", {"title": S, "text": S}, ["text"]),
    _tool("lock_screen", "Lock the computer."),
    _tool("list_packages", "List apps installed from packages."),
    _tool("install_package", "Install an app from a .jpk package file.", {"path": S}, ["path"]),
    _tool("uninstall_app", "Uninstall an app that was installed from a package.", {"name": S}, ["name"]),
    _tool("run_program", "Run an installed app by name, or a program file by path.", {"path": S}, ["path"]),
    _tool("power", "Shut down or restart the computer.", {"action": {"type": "string", "enum": ["shutdown", "restart"]}}, ["action"]),
    _tool("web_search", "Search the internet: news, facts, people, anything not on this PC.", {"query": S}, ["query"]),
    _tool("read_webpage", "Read the text of a web page.", {"url": S}, ["url"]),
]


# ==========================================================================
# Internet tools. They run here on the host, which has a full HTTPS stack
# (JARVIS OS itself only speaks plain http so far).
# ==========================================================================
import html as _html
import re as _re
import urllib.parse
import urllib.request
from html.parser import HTMLParser

_UA = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"}


class _DDGParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.results: list[dict] = []
        self._in = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        cls = a.get("class") or ""
        if tag == "a" and "result__a" in cls:
            href = a.get("href", "")
            q = urllib.parse.parse_qs(urllib.parse.urlparse(href).query)
            self.results.append({"title": "", "url": q.get("uddg", [href])[0], "snippet": ""})
            self._in = "title"
        elif "result__snippet" in cls and self.results:
            self._in = "snippet"

    def handle_endtag(self, tag):
        if tag in ("a", "td", "div"):
            self._in = None

    def handle_data(self, data):
        if self._in and self.results:
            self.results[-1][self._in] += data


def web_search(query: str) -> str:
    url = "https://html.duckduckgo.com/html/?" + urllib.parse.urlencode({"q": query})
    page = urllib.request.urlopen(urllib.request.Request(url, headers=_UA), timeout=15).read().decode("utf-8", "ignore")
    p = _DDGParser()
    p.feed(page)
    found = [r for r in p.results if r["title"].strip() and "duckduckgo.com/y.js" not in r["url"]][:5]
    if not found:
        return "error: the search returned no results"
    return "\n".join(f"{i + 1}. {r['title'].strip()} ({urllib.parse.urlparse(r['url']).netloc}): "
                     f"{' '.join(r['snippet'].split())}" for i, r in enumerate(found))


def read_webpage(url: str) -> str:
    if not url.startswith(("http://", "https://")):
        url = "https://" + url
    raw = urllib.request.urlopen(urllib.request.Request(url, headers=_UA), timeout=15).read(600000)
    text = raw.decode("utf-8", "ignore")
    text = _re.sub(r"(?is)<(script|style|noscript|svg|header|footer|nav)[^>]*>.*?</\1>", " ", text)
    text = _html.unescape(_re.sub(r"(?s)<[^>]+>", " ", text))
    return " ".join(text.split())[:2500] or "error: the page had no readable text"


HOST_TOOLS = {
    "web_search": lambda a: web_search(str(a.get("query", ""))),
    "read_webpage": lambda a: read_webpage(str(a.get("url", ""))),
}


class LlamaServer:
    """llama.cpp's server, started once and kept running."""

    def __init__(self, model: Path, threads: int, ctx: int = 4096):
        self.model = model
        self.proc = None
        if self.healthy():
            return                                   # already running (e.g. a previous bridge)
        if not LLAMA_SERVER.exists():
            raise RuntimeError(f"llama-server is not built ({LLAMA_SERVER})")
        if not model.exists():
            raise RuntimeError(f"model not found: {model}")
        log = open(Path(os.environ.get("TMPDIR", "/tmp")) / "jarvis-llama.log", "ab")
        self.proc = subprocess.Popen(
            [str(LLAMA_SERVER), "-m", str(model), "--host", "127.0.0.1", "--port", str(PORT),
             "-c", str(ctx), "-t", str(threads), "-tb", str(os.cpu_count() or 4), "-np", "1", "--cache-reuse", "256"],
            stdout=log, stderr=log, stdin=subprocess.DEVNULL)

    def healthy(self) -> bool:
        try:
            c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=2)
            c.request("GET", "/health")
            ok = c.getresponse().status == 200
            c.close()
            return ok
        except OSError:
            return False

    def wait(self, timeout: float = 300) -> bool:
        end = time.time() + timeout
        while time.time() < end:
            if self.healthy():
                return True
            if self.proc and self.proc.poll() is not None:
                return False
            time.sleep(1)
        return False

    def complete(self, messages: list, schema: dict | None = None, max_tokens: int = 120) -> str:
        """Chat completion; with a schema the output is forced to match it."""
        req = {"messages": messages, "temperature": 0.1, "max_tokens": max_tokens, "cache_prompt": True}
        if schema:
            req["response_format"] = {"type": "json_object", "schema": schema}
        c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=300)
        try:
            c.request("POST", "/v1/chat/completions", json.dumps(req).encode(), {"Content-Type": "application/json"})
            data = json.loads(c.getresponse().read().decode() or "{}")
        finally:
            c.close()
        if "choices" not in data:
            raise RuntimeError(str(data.get("error", data))[:200])
        return data["choices"][0]["message"].get("content") or ""

    def close(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


INFO_TOOLS = {"list_files", "find_files", "read_file", "system_status", "list_processes", "list_devices",
              "list_drives", "network_info", "list_apps", "list_packages", "web_search", "read_webpage",
              "device_details", "list_disks", "disk_check_status", "get_settings", "run_command"}


def tool_catalogue() -> str:
    """One line per tool: name(arg, arg) - what it does."""
    lines = []
    for t in TOOLS:
        f = t["function"]
        args = ", ".join(f["parameters"]["properties"].keys())
        lines.append(f"{f['name']}({args}) - {f['description']}")
    return "\n".join(lines)


DECIDE_PROMPT = """You are JARVIS, the voice assistant of JARVIS OS. Decide how to handle the user's latest request.
Answer only with JSON: {"tool": <tool name or "none">, "args": {...}, "say": <text>}.
- If the request is about this PC (files, apps, settings, processes, devices, network, memory, time), pick the ONE tool that does it, fill its args, and set say to "".
- For current events, facts about people, places, prices or anything you are not sure of, use web_search.
- Otherwise use "tool": "none" and put your answer in say: one or two short spoken sentences, no markdown.
- You have full control of this PC, but only delete, overwrite, end processes, uninstall or shut down when the user clearly asked for exactly that.
- The words come from speech recognition and may be misheard: pick the app or command that sounds closest (e.g. "not bad" means Notepad).
Folders: Documents, Downloads, Desktop, Pictures (write paths like "Documents/notes.txt"). USB drives: /jarvis/usb0 (read and write).
Settings (brightness, wallpaper, night light, volume, resolution, theme ...) all go through change_setting.
Tools:
""" + tool_catalogue()

ANSWER_PROMPT = """You are JARVIS, the voice assistant of JARVIS OS. Answer the user's question in one or two short spoken sentences, using only the information given. No markdown, no lists, no file paths unless asked."""

DECISION_SCHEMA = {
    "type": "object",
    "properties": {
        "tool": {"type": "string", "enum": [t["function"]["name"] for t in TOOLS] + ["none"]},
        "args": {"type": "object"},
        "say": {"type": "string"},
    },
    "required": ["tool", "args", "say"],
}


class Brain:
    """One request = one decision. The model chooses a single tool (or none)
    in constrained JSON, so it cannot loop or chain unasked actions; JARVIS OS
    runs the tool; questions get a short second pass that words the result.
    `run_tool(name, args) -> str` executes a tool in JARVIS OS."""

    def __init__(self, server: "LlamaServer", run_tool, prefs: dict | None = None):
        self.server = server
        self.run_tool = run_tool
        self.prefs = prefs if prefs is not None else {"web": True}
        self.history: list[dict] = []                # recent exchanges, for follow-ups

    def ask(self, text: str) -> str:
        if self.prefs.pop("forget", False):         # Settings > Privacy: clear history
            self.history = []
        context = self.history[-6:]
        decision = self.server.complete(
            [{"role": "system", "content": DECIDE_PROMPT}] + context + [{"role": "user", "content": text}],
            schema=DECISION_SCHEMA, max_tokens=120)
        try:
            d = json.loads(decision)
        except json.JSONDecodeError:
            d = {"tool": "none", "args": {}, "say": decision}
        tool, args, say = d.get("tool", "none"), d.get("args") or {}, (d.get("say") or "").strip()

        if tool == "none":
            answer = say or "Sorry, I am not sure how to help with that."
        else:
            if tool in HOST_TOOLS and not self.prefs.get("web", True):
                result = "error: internet search is switched off in Settings > Privacy & security"
            elif tool in HOST_TOOLS:
                try:
                    result = HOST_TOOLS[tool](args)
                except Exception as exc:
                    result = f"error: the internet request failed ({exc})"
                print(f"[brain] {tool} {json.dumps(args)[:100]} -> {len(result)} chars", flush=True)
            else:
                result = self.run_tool(tool, args)
            if tool in INFO_TOOLS and not result.startswith("error"):
                answer = self.server.complete(
                    [{"role": "system", "content": ANSWER_PROMPT},
                     {"role": "user", "content": f"{text}\n\nInformation:\n{result[:1400]}"}],
                    max_tokens=70).strip()
            else:
                answer = result.replace("/jarvis/data/Downloads", "Downloads").replace("/jarvis/data", "Documents") \
                               .replace("/jarvis/desktop", "the Desktop").replace("error: ", "Sorry, ")
        self.history += [{"role": "user", "content": text}, {"role": "assistant", "content": answer}]
        self.history = self.history[-6:]
        return answer


class GuestLink:
    """Runs tools inside JARVIS OS over the bridge socket and waits for results."""

    def __init__(self, sock: socket.socket, lock: threading.Lock):
        self.sock = sock
        self.lock = lock
        self.ids = itertools.count(1)
        self.waiting: dict[str, queue.Queue] = {}

    def send(self, line: str) -> None:
        with self.lock:
            self.sock.sendall((line.replace("\n", " ") + "\n").encode("utf-8", errors="replace"))

    def run_tool(self, name: str, args: dict) -> str:
        cid = str(next(self.ids))
        q: queue.Queue = queue.Queue()
        self.waiting[cid] = q
        print(f"[brain] tool {name} {json.dumps(args)[:120]}", flush=True)
        self.send(f"TOOL:{cid}:{name}:{json.dumps(args)}")
        try:
            return q.get(timeout=180)                 # long enough for the user to click Allow
        except queue.Empty:
            return "error: JARVIS OS did not answer (or the user did not respond)"
        finally:
            self.waiting.pop(cid, None)

    def deliver(self, line: str) -> None:
        """Handle RESULT:<id>:<text> from the guest."""
        try:
            _, cid, text = line.split(":", 2)
        except ValueError:
            return
        q = self.waiting.get(cid)
        if q:
            q.put(text.replace("\\n", "\n").replace("\\\\", "\\"))


def start_for_bridge(sock: socket.socket, sock_lock: threading.Lock, model: str | None) -> tuple[GuestLink, "queue.Queue[str]"] | None:
    """Start the model and the worker threads. Returns (link, ask_queue), or
    None if the model cannot run (JARVIS OS then uses its built-in replies)."""
    try:
        server = LlamaServer(Path(model) if model else DEFAULT_MODEL, threads=max(1, (os.cpu_count() or 4) // 2))
    except RuntimeError as exc:
        print(f"[brain] not available: {exc}", flush=True)
        return None
    link = GuestLink(sock, sock_lock)
    prefs = {"web": True}
    brain = Brain(server, link.run_tool, prefs)
    asks: "queue.Queue[str]" = queue.Queue()
    name = "Llama 3.2 3B (local)"

    def heartbeat() -> None:
        print("[brain] loading the language model...", flush=True)
        if not server.wait():
            print("[brain] llama-server did not start; see /tmp/jarvis-llama.log", flush=True)
            return
        # Read the (long) instructions once now, so they are cached and the
        # user's first request is as fast as the rest.
        started = time.time()
        try:
            server.complete([{"role": "system", "content": DECIDE_PROMPT}, {"role": "user", "content": "hello"}],
                            schema=DECISION_SCHEMA, max_tokens=8)
        except Exception as exc:
            print(f"[brain] warm-up failed: {exc}", flush=True)
        print(f"[brain] {name} is ready (instructions loaded in {time.time() - started:.0f}s)", flush=True)
        while True:
            try:
                link.send(f"AI:ONLINE {name}")
            except OSError:
                return
            time.sleep(2)

    def worker() -> None:
        while True:
            text = asks.get()
            while not asks.empty():                  # newer request: the old one is stale
                dropped, text = text, asks.get()
                print(f"[brain] skipped an older request: {dropped}", flush=True)
            started = time.time()
            try:
                link.send("AI:THINKING")
                answer = brain.ask(text)
            except Exception as exc:                  # never leave the user without a reply
                answer = f"My language model failed: {exc}"
            print(f"[brain] answered in {time.time() - started:.1f}s: {answer}", flush=True)
            try:
                link.send("REPLY:" + answer)
            except OSError:
                return

    threading.Thread(target=heartbeat, daemon=True).start()
    threading.Thread(target=worker, daemon=True).start()
    return link, asks, prefs


def main() -> int:
    print("The brain runs as part of the voice bridge: start JARVIS OS with `make run-voice`.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
