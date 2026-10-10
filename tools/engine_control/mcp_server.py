"""MCP server (stdio) that lets an agent drive a running MiniEngine editor.

Registered in the repository's .mcp.json. Tools:
    engine_launch   start miniengine_app with the control channel and wait until it answers
    engine_call     run any control command (engine_call {"cmd": "help"} lists them)
    engine_capture  capture the viewport and return it as an image
    engine_quit     close the engine
The protocol is MCP's JSON-RPC 2.0, one message a line on stdin/stdout; no SDK is needed.
"""

from __future__ import annotations

import base64
import json
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_control import DEFAULT_PORT, REPO_ROOT, EngineClient, EngineError, launch, tail  # noqa: E402

PROTOCOL_VERSION = "2025-06-18"

TOOLS = [
    {
        "name": "engine_launch",
        "description": (
            "Start the MiniEngine editor (miniengine_app) with its control channel and wait until it answers. "
            "It runs on the session's virtual desktop with --read-only-settings, so the user's settings and "
            "layout are not touched. args are extra command-line options, e.g. "
            "[\"--scene\", \"assets/scenes/vehicle_studio.yaml\", \"--viewport-size\", \"1280x720\", \"--no-audio\"]."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "args": {"type": "array", "items": {"type": "string"}, "description": "Extra engine arguments."},
                "port": {"type": "integer", "description": f"Control port (default {DEFAULT_PORT})."},
                "config": {"type": "string", "enum": ["Release", "RelWithDebInfo", "Debug"], "description": "Build to run."},
                "exe": {"type": "string", "description": "Engine executable, instead of this checkout's build."},
            },
        },
    },
    {
        "name": "engine_call",
        "description": (
            "Run a control command on the running engine and return its JSON result. cmd \"help\" lists every "
            "command with its arguments: status, frames, wait_scene, scene.load, camera.get/set, render.get/set, "
            "viewport.set, capture, photo, timings, entities.list, entity.set, drive.start/stop/reset/controls/"
            "pause/status, log, quit."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "cmd": {"type": "string"},
                "args": {"type": "object", "description": "The command's arguments."},
                "port": {"type": "integer"},
                "timeout_s": {"type": "number", "description": "Seconds to wait for the answer (default: no limit)."},
            },
            "required": ["cmd"],
        },
    },
    {
        "name": "engine_capture",
        "description": (
            "Capture the engine's viewport after `frames` more frames (default 1) and return the PNG as an image. "
            "Saved under out/control/ unless `path` is given."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "frames": {"type": "integer"},
                "port": {"type": "integer"},
            },
        },
    },
    {
        "name": "engine_quit",
        "description": "Close the engine.",
        "inputSchema": {"type": "object", "properties": {"port": {"type": "integer"}}},
    },
]

_clients: dict[int, EngineClient] = {}


def client_for(port: int) -> EngineClient:
    client = _clients.get(port)
    if client is None:
        client = EngineClient(port)
        _clients[port] = client
    return client


def drop_client(port: int) -> None:
    client = _clients.pop(port, None)
    if client is not None:
        try:
            client.close()
        except OSError:
            pass


def call(port: int, cmd: str, timeout: float | None = None, **args):
    try:
        return client_for(port).call(cmd, timeout=timeout, **args)
    except (ConnectionError, OSError):
        # A connection from before an engine restart: connect again once.
        drop_client(port)
        return client_for(port).call(cmd, timeout=timeout, **args)


def text(value) -> dict:
    return {"type": "text", "text": value if isinstance(value, str) else json.dumps(value, indent=2, ensure_ascii=False)}


def run_tool(name: str, arguments: dict) -> list[dict]:
    port = int(arguments.get("port") or DEFAULT_PORT)
    if name == "engine_launch":
        drop_client(port)
        client = launch(arguments.get("args") or [], port=port, exe=arguments.get("exe"), config=arguments.get("config") or "Release")
        _clients[port] = client
        result = client.call("ping")
        result["log"] = str(client.log_path)
        return [text(result)]
    if name == "engine_call":
        timeout = arguments.get("timeout_s")
        return [text(call(port, arguments["cmd"], timeout=timeout, **(arguments.get("args") or {})))]
    if name == "engine_capture":
        frames = int(arguments.get("frames") or 1)
        if frames > 0:
            call(port, "frames", count=frames)
        path = arguments.get("path") or str(REPO_ROOT / "out" / "control" / f"capture-{time.strftime('%Y%m%d-%H%M%S')}.png")
        result = call(port, "capture", path=path)
        data = base64.b64encode(Path(result["path"]).read_bytes()).decode("ascii")
        return [text(result), {"type": "image", "data": data, "mimeType": "image/png"}]
    if name == "engine_quit":
        try:
            call(port, "quit")
        finally:
            drop_client(port)
        return [text("quit sent")]
    raise ValueError(f"Unknown tool {name}")


def handle(message: dict) -> dict | None:
    method = message.get("method")
    request_id = message.get("id")
    if request_id is None:
        return None  # a notification (notifications/initialized and the like)
    if method == "initialize":
        requested = (message.get("params") or {}).get("protocolVersion") or PROTOCOL_VERSION
        result = {
            "protocolVersion": requested,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "miniengine-control", "version": "1.0"},
        }
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        params = message.get("params") or {}
        try:
            result = {"content": run_tool(params.get("name", ""), params.get("arguments") or {})}
        except EngineError as error:
            result = {"content": [text(f"engine error: {error}")], "isError": True}
        except Exception as error:  # noqa: BLE001 - every failure goes back to the agent as text
            detail = f"{type(error).__name__}: {error}"
            if isinstance(error, (ConnectionError, OSError)):
                log = REPO_ROOT / "out" / "control" / f"engine-{DEFAULT_PORT}.log"
                detail += "\nIs the engine running (engine_launch)?\n" + tail(log, 20)
            elif not isinstance(error, (RuntimeError, TimeoutError, ValueError, FileNotFoundError)):
                detail += "\n" + traceback.format_exc()
            result = {"content": [text(detail)], "isError": True}
    else:
        return {"jsonrpc": "2.0", "id": request_id, "error": {"code": -32601, "message": f"Unknown method {method}"}}
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def main() -> None:
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        response = handle(message)
        if response is not None:
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
