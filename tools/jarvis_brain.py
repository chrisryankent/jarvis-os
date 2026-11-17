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


def _log(msg: str, **_kw) -> None:
    """print() that cannot raise (non-blocking terminal, closed pipe)."""
    try:
        print(msg, flush=True)
    except (BlockingIOError, OSError, ValueError):
        pass
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
B = {"type": "boolean"}
def _enum(*v):
    return {"type": "string", "enum": list(v)}

APPS = ("Files, Terminal, Settings, Calculator, Tasks, Notepad, Apps, Security, Disks, Device Manager, Photos, "
        "Clock, Run, Browser, Registry Editor, JARVIS")
TOOLS = [
    # --- any app: the AI drives the desktop like a person would
    _tool("open_app", f"Open an app ({APPS}) or bring it to the front. 'my computer' means Files.", {"name": S}, ["name"]),
    _tool("window", "Manage windows: open, close, focus, minimize, maximize, restore, snap_left, snap_right, show_desktop, list.",
          {"action": _enum("open", "close", "focus", "minimize", "maximize", "restore", "snap_left", "snap_right", "show_desktop", "list"),
           "name": S}, ["action"]),
    _tool("read_screen", "Read what an app's window shows right now (all its text: buttons, fields, lists, document, page). "
          "No app = the focused window. Use it to see results or to find what to click.", {"app": S}),
    _tool("click", "Click a button, menu, tab, link, list item or text shown in an app's window, by its visible text. "
          "double for opening items in Files.", {"app": S, "text": S, "double": B, "right": B}, ["text"]),
    _tool("type_text", "Type text into an app as if on the keyboard (into its focused field). \\n = Enter.", {"app": S, "text": S}, ["text"]),
    _tool("press_keys", "Press keys or shortcuts in an app, e.g. 'ctrl+s', 'enter', 'tab', 'alt+f4', 'ctrl+a delete', 'down down enter'.",
          {"app": S, "keys": S}, ["keys"]),
    # --- apps with direct control
    _tool("notepad", "Notepad: type (at the caret), append, write (replace all text), read, save, save_as (path = file name, "
          "e.g. 'eddy' or 'Documents/notes.txt'), new, open (path), undo, redo, clear, close, status. "
          "discard true only if the user agreed to lose unsaved changes.",
          {"action": _enum("type", "append", "write", "read", "save", "save_as", "new", "open", "undo", "redo", "clear", "close", "status"),
           "text": S, "path": S, "discard": B}, ["action"]),
    _tool("browser", "The Browser: open (url), search (text), read (the page text), links, click (link or button text), "
          "fill / submit (text into the page's search box), find (text on the page), scroll (down/up/top/bottom), back, forward, "
          "reload, home, history, zoom (in/out/reset), favorite, download (url), status.",
          {"action": _enum("open", "search", "read", "links", "click", "fill", "submit", "find", "scroll", "back", "forward",
                           "reload", "home", "history", "zoom", "favorite", "download", "status"), "text": S}, ["action"]),
    _tool("terminal", "Run a command in the visible Terminal window and get its output (ls, cd, cat, apt install, apt update, "
          "dpkg, ping, ps, df, git ...). Software installs: 'apt install -y <package>'.", {"command": S}, ["command"]),
    _tool("open_settings", "Open Settings at a page (system, display, bluetooth, network, personalization, apps, accounts, "
          "time, privacy, update ...).", {"page": S}, ["page"]),
    _tool("change_setting", "Change a system setting. Names: brightness (10-100, up, down), night_light (on/off), resolution "
          "(e.g. 1920x1080), theme (dark/light/high_contrast), accent (blue/red/green/gold/purple/orange), wallpaper (bloom, aurora, "
          "mountains, dunes, night, waves, solid, or a picture path), transparency, taskbar_align (center/left), desktop_icons, "
          "clock_24h, clock_seconds, volume (0-100, up, down), mute, voice_volume, mouse_speed (slow/normal/fast/fastest), "
          "scroll_lines, dnd, lock_after_min, screen_off_min, airplane, device_name, autoplay, search_engine.",
          {"name": S, "value": S}, ["name", "value"]),
    _tool("get_settings", "All system settings and their current values."),
    # --- files
    _tool("list_files", "List a folder.", {"path": S}, ["path"]),
    _tool("find_files", "Find files by part of their name.", {"name": S}, ["name"]),
    _tool("read_file", "Read a text file.", {"path": S}, ["path"]),
    _tool("write_file", "Create or replace a text file without opening it.", {"path": S, "text": S}, ["path", "text"]),
    _tool("open_file", "Open a file in its app (text in Notepad), or a folder in Files.", {"path": S}, ["path"]),
    _tool("create_folder", "Create a folder.", {"path": S}, ["path"]),
    _tool("delete", "Move a file or folder to the Recycle Bin.", {"path": S}, ["path"]),
    _tool("move", "Move or rename a file or folder.", {"from": S, "to": S}, ["from", "to"]),
    _tool("copy", "Copy a file or folder.", {"from": S, "to": S}, ["from", "to"]),
    # --- system
    _tool("system_status", "Time, date, memory, disk, network."),
    _tool("list_processes", "Running processes with PID."),
    _tool("kill_process", "End a process by PID.", {"pid": I}, ["pid"]),
    _tool("run_command", "Run a shell command quietly (not shown in the Terminal) and get the output.", {"command": S}, ["command"]),
    _tool("list_devices", "Hardware devices, drivers, status."),
    _tool("device_action", "Device Manager: info, enable, disable, restart, uninstall, scan, events, driver.",
          {"action": _enum("info", "enable", "disable", "restart", "uninstall", "scan", "events", "driver"), "name": S}, ["action"]),
    _tool("list_disks", "Disks, volumes, free space, errors."),
    _tool("disk_action", "Disk management: list, info, create, format, label, extend, shrink, delete, init. format/delete/init "
          "erase data: confirm true only after the user agreed.",
          {"action": _enum("list", "info", "create", "format", "label", "extend", "shrink", "delete", "init"),
           "disk": S, "volume": S, "size_mb": I, "label": S, "fs": _enum("fat32", "fat16", "auto"), "confirm": B}, ["action"]),
    _tool("check_disk", "Check the disk for errors in the background.", {"repair": B, "surface": B}),
    _tool("eject_drive", "Safely eject a USB drive.", {"name": S}),
    _tool("network_info", "Network connection details."),
    _tool("screenshot", "Take a screenshot (saved in Pictures/Screenshots)."),
    _tool("notify", "Show a notification.", {"title": S, "text": S}, ["text"]),
    _tool("lock_screen", "Lock the computer."),
    _tool("power", "Shut down or restart.", {"action": _enum("shutdown", "restart")}, ["action"]),
    _tool("list_packages", "Apps installed from .jpk packages."),
    _tool("install_package", "Install a .jpk app package file.", {"path": S}, ["path"]),
    _tool("uninstall_app", "Uninstall a .jpk app.", {"name": S}, ["name"]),
    # --- internet (runs on the host)
    _tool("web_search", "Quick internet search: headlines and snippets.", {"query": S}, ["query"]),
    _tool("research", "Research a question on the internet: searches, reads the best pages and returns the facts. Use for "
          "explanations, news, prices, how-to, people, anything needing detail.", {"query": S}, ["query"]),
    _tool("read_webpage", "Read the text of a web page by its address.", {"url": S}, ["url"]),
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


def _page_text(url: str, limit: int = 600000) -> str:
    raw = urllib.request.urlopen(urllib.request.Request(url, headers=_UA), timeout=12).read(limit)
    text = raw.decode("utf-8", "ignore")
    text = _re.sub(r"(?is)<(script|style|noscript|svg|header|footer|nav|aside|form)[^>]*>.*?</\1>", " ", text)
    text = _re.sub(r"(?i)</(p|div|li|h[1-6]|tr|br)>", "\n", text)
    return _html.unescape(_re.sub(r"(?s)<[^>]+>", " ", text))


def research(query: str) -> str:
    """Search, then read the top pages in parallel and keep the sentences
    that answer the question: real page text, with where it came from."""
    url = "https://html.duckduckgo.com/html/?" + urllib.parse.urlencode({"q": query})
    page = urllib.request.urlopen(urllib.request.Request(url, headers=_UA), timeout=15).read().decode("utf-8", "ignore")
    p = _DDGParser()
    p.feed(page)
    found = [r for r in p.results if r["title"].strip() and "duckduckgo.com/y.js" not in r["url"]][:6]
    if not found:
        return "error: the search returned no results"
    words = {w for w in _re.findall(r"[a-z0-9]{3,}", query.lower())} - {"the", "and", "for", "what", "how", "who", "with", "about", "does"}
    pages: dict[int, str] = {}

    def fetch(i: int, u: str) -> None:
        try:
            pages[i] = _page_text(u)
        except Exception:
            pass
    threads = [threading.Thread(target=fetch, args=(i, r["url"]), daemon=True) for i, r in enumerate(found[:3])]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=12)
    out = []
    for i, r in enumerate(found):
        site = urllib.parse.urlparse(r["url"]).netloc
        snippet = " ".join(r["snippet"].split())
        text = pages.get(i, "")
        best = []
        if text:
            sentences = _re.split(r"(?<=[.!?])\s+|\n+", text)
            scored = []
            for k, sent in enumerate(sentences):
                sent = " ".join(sent.split())
                if not 40 <= len(sent) <= 400:
                    continue
                score = sum(1 for w in words if w in sent.lower())
                if score:
                    scored.append((score, -k, sent))
            best = [x[2] for x in sorted(scored, reverse=True)[:4]]
        body = " ".join(best) if best else snippet
        if body:
            out.append(f"[{site}] {r['title'].strip()}: {body}")
    return "\n".join(out)[:3200] or "error: the pages had no readable text"


HOST_TOOLS = {
    "web_search": lambda a: web_search(str(a.get("query", ""))),
    "research": lambda a: research(str(a.get("query", ""))),
    "read_webpage": lambda a: read_webpage(str(a.get("url", ""))),
}


def physical_cores() -> int:
    """Real cores (not hyper-threads); llama.cpp is slower with more threads than cores."""
    try:
        cores = set()
        phys = core = None
        for line in open("/proc/cpuinfo", encoding="utf-8"):
            if line.startswith("physical id"):
                phys = line.split(":")[1].strip()
            elif line.startswith("core id"):
                core = line.split(":")[1].strip()
                cores.add((phys, core))
        if cores:
            return max(1, len(cores))
    except OSError:
        pass
    return max(1, (os.cpu_count() or 4) // 2)


BRAIN_SERVER: list = [None]                           # for shutdown by the bridge


class LlamaServer:
    """llama.cpp's server, started once and kept running."""

    def __init__(self, model: Path, threads: int, ctx: int = 6144):
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
             # Physical cores only: hyper-threads and other busy programs
             # (QEMU, speech recognition) made generation 3x slower. The KV
             # cache is 8-bit so a long plan plus web results fit in RAM.
             "-c", str(ctx), "-t", str(threads), "-tb", str(threads), "-np", "1", "--cache-reuse", "256",
             "-fa", "on", "-ctk", "q8_0", "-ctv", "q8_0"],
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


# Tools whose result the user wants to hear about (worded by the model).
INFO_TOOLS = {"list_files", "find_files", "read_file", "system_status", "list_processes", "list_devices",
              "network_info", "web_search", "research", "read_webpage", "list_disks", "get_settings", "run_command",
              "read_screen", "terminal", "list_packages"}
INFO_ACTIONS = {("browser", "read"), ("browser", "links"), ("notepad", "read"), ("window", "list")}


def tool_catalogue() -> str:
    """One line per tool: name(arg, arg) - what it does."""
    lines = []
    for t in TOOLS:
        f = t["function"]
        args = ", ".join(f["parameters"]["properties"].keys())
        lines.append(f"{f['name']}({args}) - {f['description']}")
    return "\n".join(lines)


# One system prompt for every call: llama-server keeps it cached, so only
# the new words of each request are read (a second prompt for wording the
# answer used to evict it, and each request then re-read ~1500 tokens).
SYSTEM_PROMPT = """You are JARVIS, the AI that drives JARVIS OS for its owner. You operate every app on this PC the way a person would: open apps, type, click, press keys, read the screen, save files, run Terminal commands, browse and research the internet.
Answer each request with JSON only: {"intent": "act" | "answer" | "ignore", "steps": [{"tool": ..., "args": {...}}, ...], "say": ...}
- act: list the steps that do EVERY part of the request, in order (at most 6); "and save it as X" needs its own save_as step. Example "open notepad, write a shopping list with milk and eggs and save it as shopping": [{"tool":"notepad","args":{"action":"write","text":"Shopping list\\n- milk\\n- eggs"}}, {"tool":"notepad","args":{"action":"save_as","path":"shopping"}}]. The notepad tool opens Notepad itself.
- To work in an app with no special tool, use open_app, then read_screen to see it, click / type_text / press_keys.
- "save it", "type ...", "write ..." right after Notepad was used mean the Notepad document. Writing a letter, story, list or notes: compose the full text yourself in the step's text.
- Use "{result}" inside an argument to insert the previous step's result (e.g. research, then notepad write {"text":"{result}"}).
- Questions about the internet, news, people, prices, how-to: research (or web_search for a quick fact). Installing Linux software: terminal "apt install -y <package>" (run "apt update" first if the user has never done it).
- answer: no tool needed (greetings, opinions, things you know): steps [], say = one or two short spoken sentences.
- ignore: ONLY for words that are not a sentence meant for you, e.g. "It as in.", "The", "As ending.", "Oh no no no", a TV line, or someone talking to another person. Never guess an action from such words. Greetings, questions and every clear request are never ignored ("install wifite" = act, "hello" = answer).
- Only delete, overwrite, end processes, uninstall, format or shut down when the user clearly asked for exactly that.
- Words come from speech recognition and may be misheard ("not bad" = Notepad).
Folders: Documents, Downloads, Desktop, Pictures (paths like "Documents/notes.txt", or just a name like "eddy" for Documents). USB: /kryzen/usb0.
Tools:
""" + tool_catalogue()

WORDING_NOTE = ("Results of the steps you took are below. Reply to the user now as JSON {\"say\": ...}: plain spoken "
                "sentences, no markdown or lists, using only these results. Say what was actually done; if a step failed, "
                "say so and why. For research, give the facts asked for in two to four sentences.")

STEP_SCHEMA = {
    "type": "object",
    "properties": {
        "tool": {"type": "string", "enum": [t["function"]["name"] for t in TOOLS]},
        "args": {"type": "object"},
    },
    "required": ["tool", "args"],
}
DECISION_SCHEMA = {
    "type": "object",
    "properties": {
        "intent": {"type": "string", "enum": ["act", "answer", "ignore"]},
        "steps": {"type": "array", "items": STEP_SCHEMA, "maxItems": 6},
        "say": {"type": "string"},
    },
    "required": ["intent", "steps", "say"],
}
SAY_SCHEMA = {"type": "object", "properties": {"say": {"type": "string"}}, "required": ["say"]}


def _friendly(text: str) -> str:
    """Paths and error prefixes as a person would say them."""
    for path, name in (("/kryzen/data/Downloads", "Downloads"), ("/kryzen/data/Pictures", "Pictures"),
                       ("/kryzen/data", "Documents"), ("/kryzen/desktop", "the Desktop"), ("/kryzen/trash", "the Recycle Bin")):
        text = text.replace(path, name)
    return text.replace("error: ", "Sorry, ")


def _first_sentences(text: str, n: int = 2) -> str:
    text = " ".join(text.split())
    parts = _re.split(r"(?<=[.!?])\s+", text)
    return " ".join(parts[:n])


class Brain:
    """One request = one plan. The model writes the whole plan in one
    constrained JSON reply (a 3B model loops when it is asked to call tools
    one at a time), JARVIS OS runs the steps in order, stopping at the
    first failure, and the reply is built from what really happened.
    `run_tool(name, args) -> str` executes a tool in JARVIS OS."""

    def __init__(self, server: "LlamaServer", run_tool, prefs: dict | None = None, log=None):
        self.server = server
        self.run_tool = run_tool
        self.prefs = prefs if prefs is not None else {"web": True}
        self.history: list[dict] = []                # recent exchanges, for follow-ups
        self.log = log or _log

    def _run_step(self, tool: str, args: dict) -> str:
        if tool in HOST_TOOLS:
            if not self.prefs.get("web", True):
                return "error: internet search is switched off in Settings > Privacy & security"
            try:
                result = HOST_TOOLS[tool](args)
            except Exception as exc:
                result = f"error: the internet request failed ({exc})"
            self.log(f"[brain] {tool} {json.dumps(args)[:100]} -> {len(result)} chars")
            return result
        result = self.run_tool(tool, args)
        # a page or command still working: wait for it, as a person would
        waited = 0.0
        while result.startswith("LOADING:") and waited < 25:
            time.sleep(1.5)
            waited += 1.5
            result = self.run_tool(tool, args if args.get("action") in ("read", "links", "status") else {"action": "status"})
        if tool == "browser" and args.get("action") in ("open", "search", "back", "forward", "click", "submit", "reload") \
                and not result.startswith("error"):
            for _ in range(16):                      # report the page that actually opened
                time.sleep(1.5)
                status = self.run_tool("browser", {"action": "status"})
                if not status.startswith("LOADING:"):
                    result = status
                    break
        return result

    def ask(self, text: str) -> str:
        if self.prefs.pop("forget", False):         # Settings > Privacy: clear history
            self.history = []
        messages = [{"role": "system", "content": SYSTEM_PROMPT}] + self.history[-4:] + [{"role": "user", "content": text}]
        decision = self.server.complete(messages, schema=DECISION_SCHEMA, max_tokens=700)
        try:
            d = json.loads(decision)
        except json.JSONDecodeError:
            d = {"intent": "answer", "steps": [], "say": ""}
        intent = d.get("intent", "answer")
        steps = [st for st in (d.get("steps") or []) if isinstance(st, dict) and st.get("tool")]
        say = (d.get("say") or "").strip()

        if intent == "ignore" and not steps:
            self.log(f"[brain] not a request for JARVIS, ignored: {text}")
            return ""
        if not steps:
            answer = say or "Sorry, I didn't catch that. Could you say it again?"
            self._remember(text, answer)
            return answer

        results: list[tuple[str, dict, str]] = []
        previous = ""
        for st in steps[:6]:
            tool, args = st["tool"], dict(st.get("args") or {})
            for k, v in list(args.items()):
                if isinstance(v, str):
                    # the model sometimes escapes twice: "\\n" meant a new line
                    v = v.replace("\\n", "\n").replace("\\t", "\t")
                    if "{result}" in v:                # the previous step's output
                        v = v.replace("{result}", previous)
                    args[k] = v
            result = self._run_step(tool, args)
            results.append((tool, args, result))
            previous = result
            if result.startswith("error") or result.startswith("The user denied"):
                break

        wants_words = any(t in INFO_TOOLS or (t, a.get("action")) in INFO_ACTIONS for t, a, _ in results) \
            or any(r.startswith("error") for _, _, r in results)
        if wants_words:
            report = "\n".join(f"{i + 1}. {t} {json.dumps(a)[:160]} -> {r[:2400 if t in ('research', 'read_screen') else 1200]}"
                               for i, (t, a, r) in enumerate(results))
            worded = self.server.complete(
                messages + [{"role": "assistant", "content": decision},
                            {"role": "user", "content": WORDING_NOTE + "\n" + report}],
                schema=SAY_SCHEMA, max_tokens=160)
            try:
                answer = json.loads(worded).get("say", "").strip()
            except json.JSONDecodeError:
                answer = worded.strip()
            if not answer:
                answer = _first_sentences(results[-1][2])
        else:
            # Actions: the OS's own reports, so nothing is claimed that did not happen.
            answer = " ".join(_first_sentences(r, 1) for _, _, r in results)
        answer = _friendly(answer)
        self._remember(text, answer, results)
        return answer

    def _remember(self, text: str, answer: str, results=None) -> None:
        done = ""
        if results:
            done = " [did: " + "; ".join(f"{t} {json.dumps(a)[:80]}" for t, a, _ in results) + "]"
        self.history += [{"role": "user", "content": text},
                         {"role": "assistant", "content": json.dumps({"intent": "act" if results else "answer",
                                                                      "steps": [], "say": answer + done})}]
        self.history = self.history[-4:]


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
        _log(f"[brain] tool {name} {json.dumps(args)[:120]}", flush=True)
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
        server = LlamaServer(Path(model) if model else DEFAULT_MODEL, threads=physical_cores())
    except RuntimeError as exc:
        _log(f"[brain] not available: {exc}", flush=True)
        return None
    link = GuestLink(sock, sock_lock)
    prefs = {"web": True}
    brain = Brain(server, link.run_tool, prefs)
    BRAIN_SERVER[0] = server
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
            server.complete([{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": "hello"}],
                            schema=DECISION_SCHEMA, max_tokens=8)
        except Exception as exc:
            _log(f"[brain] warm-up failed: {exc}", flush=True)
        _log(f"[brain] {name} is ready (instructions loaded in {time.time() - started:.0f}s)", flush=True)
        while True:
            try:
                link.send(f"AI:ONLINE {name}")
            except OSError:
                return
            time.sleep(2)

    def log(msg: str) -> None:
        """Print without ever raising: the terminal may be in non-blocking mode
        (QEMU sets it), and a failed print must not kill the brain."""
        try:
            print(msg, flush=True)
        except (BlockingIOError, OSError, ValueError):
            pass

    def worker() -> None:
        while True:
            try:
                text = asks.get()
                while not asks.empty():              # newer request: the old one is stale
                    dropped, text = text, asks.get()
                    log(f"[brain] skipped an older request: {dropped}")
                started = time.time()
                try:
                    link.send("AI:THINKING")
                    answer = brain.ask(text)
                except Exception as exc:              # never leave the user without a reply
                    answer = f"My language model failed: {exc}"
                log(f"[brain] answered in {time.time() - started:.1f}s: {answer}")
                try:
                    link.send("REPLY:" + answer)
                except OSError:
                    return
            except Exception as exc:                  # keep answering whatever happens
                log(f"[brain] request failed: {exc}")

    try:                                              # QEMU leaves stdout non-blocking
        os.set_blocking(sys.stdout.fileno(), True)
    except (OSError, ValueError, AttributeError):
        pass
    threading.Thread(target=heartbeat, daemon=True).start()
    threading.Thread(target=worker, daemon=True).start()
    return link, asks, prefs


def main() -> int:
    print("The brain runs as part of the voice bridge: start JARVIS OS with `make run-voice`.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
