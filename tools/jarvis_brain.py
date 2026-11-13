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
    _tool("open_app", "Open an app (Files, Terminal, Settings, Calculator, Tasks, Notepad, Apps, Security).", {"name": S}, ["name"]),
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
             "-c", str(ctx), "-t", str(threads), "-np", "1", "--cache-reuse", "256"],
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
              "list_drives", "network_info", "list_apps", "list_packages"}


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
- Otherwise use "tool": "none" and put your answer in say: one or two short spoken sentences, no markdown.
- Only use delete, write_file over an existing file, kill_process, uninstall_app or power when the user clearly asked for exactly that.
Folders: Documents, Downloads, Desktop (write paths like "Documents/notes.txt"). USB drives: /jarvis/usb0 (read-only).
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

    def __init__(self, server: "LlamaServer", run_tool):
        self.server = server
        self.run_tool = run_tool
        self.history: list[dict] = []                # recent exchanges, for follow-ups

    def ask(self, text: str) -> str:
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
            result = self.run_tool(tool, args)
            if tool in INFO_TOOLS and not result.startswith("error"):
                answer = self.server.complete(
                    [{"role": "system", "content": ANSWER_PROMPT},
                     {"role": "user", "content": f"{text}\n\nInformation:\n{result[:1800]}"}],
                    max_tokens=90).strip()
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
    brain = Brain(server, link.run_tool)
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
    print("The brain runs as part of the voice bridge: start JARVIS OS with `make run-voice`.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
