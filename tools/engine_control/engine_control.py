"""Client for the engine's control channel (miniengine_app --control).

The engine listens on 127.0.0.1 and takes one JSON request a line:
    {"id": 1, "cmd": "camera.set", "args": {"position": [0, 2, 5]}}
and answers each with one line carrying the same id. See
docs/design/2026-10-10-engine-control-channel-design.md; `help` lists the commands.

Used by mectl.py (command line) and mcp_server.py (MCP tools); import it for scripts:
    from engine_control import EngineClient, launch
    engine = launch(["--scene", "assets/scenes/vehicle_studio.yaml"])
    engine.call("wait_scene")
    engine.call("capture", path="out/control/shot.png")
"""

from __future__ import annotations

import itertools
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_PORT = 47811
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))


class EngineError(RuntimeError):
    """The engine answered a request with an error."""


class EngineClient:
    """One connection to a running engine. Requests are answered in turn; a waiting command
    (frames, wait_scene, photo) blocks call() until the engine answers it."""

    def __init__(self, port: int = DEFAULT_PORT, host: str = "127.0.0.1", connect_timeout: float = 5.0):
        self.port = port
        self._socket = socket.create_connection((host, port), timeout=connect_timeout)
        self._socket.settimeout(None)
        self._reader = self._socket.makefile("r", encoding="utf-8", newline="\n")
        self._ids = itertools.count(1)
        self._connected_at = time.time()

    def close(self) -> None:
        try:
            self._reader.close()
        finally:
            self._socket.close()

    def __enter__(self) -> "EngineClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def call(self, cmd: str, timeout: float | None = None, **args):
        """Runs `cmd` with `args` and returns its result; raises EngineError with the engine's message."""
        request_id = next(self._ids)
        line = json.dumps({"id": request_id, "cmd": cmd, "args": args}, separators=(",", ":"))
        self._socket.settimeout(timeout)
        try:
            self._socket.sendall(line.encode("utf-8") + b"\n")
            while True:
                try:
                    text = self._reader.readline()
                except ConnectionResetError:
                    text = ""
                if not text:
                    report = latest_crash_report(self._connected_at - 5.0)
                    if report is not None:
                        raise ConnectionError(f"The engine crashed. Report {report}:\n{crash_summary(report)}")
                    raise ConnectionError("The engine closed the connection (did it quit or crash?)")
                answer = json.loads(text)
                if answer.get("id") != request_id:
                    continue  # an answer to an earlier request that timed out here
                if not answer.get("ok"):
                    raise EngineError(answer.get("error", "unknown error"))
                return answer.get("result")
        finally:
            self._socket.settimeout(None)


def default_exe(config: str = "Release") -> Path:
    return REPO_ROOT / "out" / "build" / "vs2026-x64" / "app" / config / "miniengine_app.exe"


def is_listening(port: int = DEFAULT_PORT) -> bool:
    try:
        with EngineClient(port, connect_timeout=0.5) as client:
            client.call("ping", timeout=5.0)
        return True
    except OSError:
        return False


def launch(
    extra_args: list[str] | None = None,
    port: int = DEFAULT_PORT,
    exe: Path | str | None = None,
    config: str = "Release",
    log_path: Path | str | None = None,
    read_only_settings: bool = True,
    ready_timeout: float = 180.0,
) -> EngineClient:
    """Starts miniengine_app with the control channel and returns a client once it answers.

    The engine runs detached (it outlives this process), from the repository root, on the
    launching session's virtual desktop (MINIENGINE_VIRTUAL_DESKTOP=launcher), and by default
    with --read-only-settings so the user's miniengine.settings.json and imgui.ini stay as they are.
    Its console output goes to `log_path` (out/control/engine-PORT.log by default)."""
    if is_listening(port):
        raise RuntimeError(f"An engine already listens on port {port}; quit it or pick another port")
    exe_path = Path(exe) if exe else default_exe(config)
    if not exe_path.exists():
        raise FileNotFoundError(f"No engine at {exe_path}; build it first (scripts/build.sh vs2026-x64-release --target miniengine_app)")
    log_file = Path(log_path) if log_path else REPO_ROOT / "out" / "control" / f"engine-{port}.log"
    log_file.parent.mkdir(parents=True, exist_ok=True)

    command = [str(exe_path), "--control", str(port)]
    if read_only_settings:
        command.append("--read-only-settings")
    command += extra_args or []
    environment = dict(os.environ)
    environment.setdefault("MINIENGINE_VIRTUAL_DESKTOP", "launcher")
    flags = 0
    if sys.platform == "win32":
        flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    with open(log_file, "wb") as log:
        process = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            creationflags=flags,
            close_fds=True,
        )

    deadline = time.monotonic() + ready_timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"The engine exited with code {process.returncode} before it answered:\n{tail(log_file, 30)}")
        try:
            client = EngineClient(port, connect_timeout=0.5)
            client.call("ping", timeout=10.0)
            client.pid = process.pid
            client.log_path = log_file
            return client
        except OSError:
            time.sleep(0.25)
    raise TimeoutError(f"The engine did not answer on port {port} within {ready_timeout:.0f} s:\n{tail(log_file, 30)}")


def steps_from_spec(spec) -> list[tuple[str, dict]]:
    """One side of an A/B: a list of {"cmd": ..., "args": {...}} steps, or (shorthand) an object of
    render settings for render.set."""
    if spec is None:
        return []
    if isinstance(spec, dict):
        return [("render.set", {"values": spec})]
    return [(step["cmd"], step.get("args") or {}) for step in spec]


def ab_compare(
    client: EngineClient,
    a: list[tuple[str, dict]],
    b: list[tuple[str, dict]],
    frames: int = 64,
    out: Path | str | None = None,
    restore: list[tuple[str, dict]] | None = None,
) -> dict:
    """Captures the same view under two set-ups in one engine and compares them.

    `a` and `b` are lists of (command, args) that set each side up, e.g.
    [("render.set", {"values": {"ray_tracing.reflections": True}})]. Frames are made repeatable first
    (deterministic: time frozen, exposure pinned at what it is now, auto white balance off). Captures
    A, then B, then A again: A against B is the change, A against A the noise floor (what DDGI and
    other state the reset does not cover leave). Each capture follows a temporal restart and `frames`
    frames. `restore` runs at the end (default: A's set-up); deterministic is switched off again."""
    import image_compare  # beside this file

    folder = Path(out) if out else REPO_ROOT / "out" / "control" / f"ab-{time.strftime('%Y%m%d-%H%M%S')}"
    folder.mkdir(parents=True, exist_ok=True)
    client.call("deterministic", frame_seconds=0.0)
    try:
        captures = {}
        for name, steps in (("a", a), ("b", b), ("a2", a)):
            for command, args in steps:
                client.call(command, **args)
            client.call("restart_temporal", full=True)
            client.call("frames", count=frames)
            captures[name] = Path(client.call("capture", path=str(folder / f"{name}.png"))["path"])
        change = image_compare.compare(captures["a"], captures["b"], folder / "a_vs_b")
        floor = image_compare.compare(captures["a"], captures["a2"], folder / "a_vs_a")
    finally:
        for command, args in restore if restore is not None else a:
            client.call(command, **args)
        client.call("deterministic", enabled=False)
    return {
        "folder": str(folder),
        "change": change,
        "noise_floor": floor,
        "summary": f"A vs B: {image_compare.summary(change)}; A vs A (floor): {image_compare.summary(floor)}",
    }


def crash_folder() -> Path:
    return Path(os.environ.get("LOCALAPPDATA", "")) / "MiniEngine" / "crashes"


def latest_crash_report(since: float) -> Path | None:
    """The newest crash report (engine/platform/crash) written after `since` (time.time())."""
    reports = [path for path in crash_folder().glob("miniengine_*.txt") if path.stat().st_mtime >= since]
    return max(reports, key=lambda path: path.stat().st_mtime) if reports else None


def crash_summary(report: Path, stack_lines: int = 25) -> str:
    """A crash report's head: the reason and the top of the stack."""
    lines = report.read_text(encoding="utf-8", errors="replace").splitlines()
    end = next((index for index, line in enumerate(lines) if line.startswith("The log's last lines")), len(lines))
    head = lines[:end]
    stack_start = next((index for index, line in enumerate(head) if line.startswith("Stack of")), len(head))
    return "\n".join(head[: stack_start + 1 + stack_lines])


def tail(path: Path, lines: int) -> str:
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return "\n".join(text.splitlines()[-lines:])
