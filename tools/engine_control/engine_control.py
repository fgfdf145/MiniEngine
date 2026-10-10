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
                text = self._reader.readline()
                if not text:
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


def tail(path: Path, lines: int) -> str:
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return "\n".join(text.splitlines()[-lines:])
