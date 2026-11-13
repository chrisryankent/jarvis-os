#!/usr/bin/env python3
"""
JARVIS brain: the language model that reasons and acts for JARVIS OS.

Runs on the host next to the voice bridge. A local model (default: the
Llama 3.2 3B Instruct in tools/models/brain/model.gguf) is served by
llama.cpp's llama-server; the user's requests arrive from the guest as
ASK: lines, and the model answers or calls tools that JARVIS OS executes
(kernel/ai_tools.c). Destructive tools wait for the user's Allow click in
JARVIS OS, so the model can never delete, kill or shut down by itself.

Used by tools/voice_bridge.py. Can be tried on its own, without JARVIS OS
(tools are then only printed, not run):

    tools/jarvis_brain.py --chat
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

SYSTEM_PROMPT = """You are JARVIS, the assistant built into JARVIS OS, a desktop operating system. You talk to your user by voice, so answer in one to three short, natural sentences, without markdown or lists.

You can act on the computer with the tools provided. Use them whenever the user asks about or wants something done on this PC: look things up instead of guessing, and act instead of explaining how. Call one tool at a time and wait for its result. After acting, say briefly what you did. If a tool returns an error, explain it plainly.

Folders: /jarvis/data is Documents, /jarvis/data/Downloads is Downloads, /jarvis/desktop is the Desktop, USB drives appear as /jarvis/usb0 and so on (read-only). Deleting moves things to the Recycle Bin. Actions such as deleting, ending a process, installing or shutting down ask the user for permission inside JARVIS OS; if the user denies, accept it.

Never invent file names, process numbers or system facts: get them with a tool."""


def _tool(name: str, description: str, props: dict | None = None, required: list[str] | None = None) -> dict:
    return {"type": "function", "function": {
        "name": name, "description": description,
        "parameters": {"type": "object", "properties": props or {}, "required": required or []}}}


S = {"type": "string"}
I = {"type": "integer"}
TOOLS = [
    _tool("open_app", "Open an app: Files, Terminal, JARVIS, Security, Settings, Calculator, Tasks, Notepad, Apps, or an installed app.", {"name": S}, ["name"]),
    _tool("close_app", "Close an open app window.", {"name": S}, ["name"]),
    _tool("list_apps", "List the apps on this PC and which are open."),
    _tool("list_files", "List the files and folders in a folder.", {"path": S}, ["path"]),
    _tool("find_files", "Find files and folders whose name contains some text, on the disk and USB drives.", {"name": S}, ["name"]),
    _tool("read_file", "Read a text file (first 3000 characters).", {"path": S}, ["path"]),
    _tool("write_file", "Create or replace a text file with the given text.", {"path": S, "text": S}, ["path", "text"]),
    _tool("create_folder", "Create a folder.", {"path": S}, ["path"]),
    _tool("delete", "Move a file or folder to the Recycle Bin.", {"path": S}, ["path"]),
    _tool("move", "Move or rename a file or folder.", {"from": S, "to": S}, ["from", "to"]),
    _tool("copy", "Copy a file or folder.", {"from": S, "to": S}, ["from", "to"]),
    _tool("open_file", "Open a file in Notepad, or a folder in Files.", {"path": S}, ["path"]),
    _tool("system_status", "Time, date, uptime, memory, disk, network and process count."),
    _tool("list_processes", "List running processes with their PID."),
    _tool("kill_process", "End a process by PID.", {"pid": I}, ["pid"]),
    _tool("suspend_process", "Pause a process by PID.", {"pid": I}, ["pid"]),
    _tool("resume_process", "Resume a paused process by PID.", {"pid": I}, ["pid"]),
    _tool("list_devices", "List the hardware devices and their drivers."),
    _tool("list_drives", "List the disks and USB drives with free space."),
    _tool("network_info", "Network connection details."),
    _tool("set_theme", "Change the look: dark, light or high_contrast.", {"mode": {"type": "string", "enum": ["dark", "light", "high_contrast"]}}, ["mode"]),
    _tool("notify", "Show a notification.", {"title": S, "text": S}, ["text"]),
    _tool("lock_screen", "Lock the computer."),
    _tool("list_packages", "List apps installed from packages."),
    _tool("install_package", "Install an app from a .jpk package file.", {"path": S}, ["path"]),
    _tool("uninstall_app", "Uninstall an app that was installed from a package.", {"name": S}, ["name"]),
    _tool("run_program", "Run an installed app by name, or a program file by path.", {"path": S}, ["path"]),
    _tool("power", "Shut down or restart the computer.", {"action": {"type": "string", "enum": ["shutdown", "restart"]}}, ["action"]),
]


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
             "-c", str(ctx), "-t", str(threads), "--jinja", "-np", "1", "--cache-reuse", "256"],
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

    def chat(self, messages: list, tools: list) -> dict:
        body = json.dumps({"messages": messages, "tools": tools, "temperature": 0.3,
                           "max_tokens": 300, "cache_prompt": True}).encode()
        c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=300)
        try:
            c.request("POST", "/v1/chat/completions", body, {"Content-Type": "application/json"})
            r = c.getresponse()
            data = json.loads(r.read().decode() or "{}")
        finally:
            c.close()
        if "choices" not in data:
            raise RuntimeError(str(data.get("error", data))[:200])
        return data["choices"][0]["message"]

    def close(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


class Brain:
    """The conversation and the tool loop. `run_tool(name, args) -> str`
    executes a tool (in JARVIS OS, through the bridge)."""

    def __init__(self, server: LlamaServer, run_tool, max_steps: int = 6):
        self.server = server
        self.run_tool = run_tool
        self.max_steps = max_steps
        self.history: list[dict] = []

    def ask(self, text: str) -> str:
        self.history.append({"role": "user", "content": text})
        self.history = self.history[-16:]            # keep the context small and fast
        while self.history and self.history[0]["role"] != "user":
            self.history.pop(0)
        messages = [{"role": "system", "content": SYSTEM_PROMPT}] + self.history
        for _ in range(self.max_steps):
            msg = self.server.chat(messages, TOOLS)
            calls = msg.get("tool_calls") or []
            if not calls:
                answer = (msg.get("content") or "").strip() or "Done."
                self.history.append({"role": "assistant", "content": answer})
                return answer
            messages.append({"role": "assistant", "content": msg.get("content") or "", "tool_calls": calls[:1]})
            call = calls[0]                           # one at a time, as instructed
            name = call["function"]["name"]
            try:
                args = json.loads(call["function"].get("arguments") or "{}")
            except json.JSONDecodeError:
                args = {}
            result = self.run_tool(name, args)
            messages.append({"role": "tool", "tool_call_id": call.get("id", "0"), "name": name, "content": result})
            self.history.extend(messages[-2:])
        return "I could not finish that in a few steps. Could you put it another way?"


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
    brain = Brain(server, link.run_tool)
    asks: "queue.Queue[str]" = queue.Queue()
    name = "Llama 3.2 3B (local)"

    def heartbeat() -> None:
        print("[brain] loading the language model...", flush=True)
        if not server.wait():
            print("[brain] llama-server did not start; see /tmp/jarvis-llama.log", flush=True)
            return
        print(f"[brain] {name} is ready", flush=True)
        while True:
            try:
                link.send(f"AI:ONLINE {name}")
            except OSError:
                return
            time.sleep(2)

    def worker() -> None:
        while True:
            text = asks.get()
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
    return link, asks


def main() -> int:
    if "--chat" not in sys.argv:
        print(__doc__)
        return 1
    server = LlamaServer(DEFAULT_MODEL, threads=max(1, (os.cpu_count() or 4) // 2))
    print("Loading the model...", flush=True)
    if not server.wait():
        print("llama-server did not start; see /tmp/jarvis-llama.log")
        return 1

    def fake_tool(name: str, args: dict) -> str:
        print(f"  [tool] {name}({json.dumps(args)})")
        return input("  result> ") or "ok"

    brain = Brain(server, fake_tool)
    try:
        while True:
            text = input("you> ").strip()
            if text:
                t0 = time.time()
                print(f"jarvis> {brain.ask(text)}   ({time.time() - t0:.1f}s)")
    except (EOFError, KeyboardInterrupt):
        print()
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
