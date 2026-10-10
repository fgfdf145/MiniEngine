"""Command line for the engine's control channel.

    python tools/engine_control/mectl.py launch [--port N] [--config Debug] [-- ENGINE ARGS...]
    python tools/engine_control/mectl.py CMD [key=value ...] [--json '{...}'] [--port N] [--timeout S]

Values in key=value are read as JSON when they parse (numbers, true, [1,2,3]) and as strings
otherwise. Examples:
    mectl.py launch -- --scene assets/scenes/vehicle_studio.yaml --viewport-size 1280x720
    mectl.py wait_scene
    mectl.py camera.set position=[0,1.5,6] look_at=[0,0.5,0]
    mectl.py render.set --json '{"values": {"ray_tracing.reflections": false}}'
    mectl.py capture path=out/control/shot.png
    mectl.py help
Prints the result as JSON; exits 1 with the engine's message on an error.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_control import DEFAULT_PORT, EngineClient, EngineError, ab_compare, launch, steps_from_spec  # noqa: E402


def parse_value(text: str):
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def main(argv: list[str]) -> int:
    # Labels carry icon-font glyphs; a GBK or cp1252 console cannot print them.
    sys.stdout.reconfigure(encoding="utf-8")
    engine_args: list[str] = []
    if "--" in argv:
        split = argv.index("--")
        argv, engine_args = argv[:split], argv[split + 1 :]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command")
    parser.add_argument("pairs", nargs="*", help="key=value arguments")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--json", dest="json_args", help="the arguments as one JSON object")
    parser.add_argument("--timeout", type=float, default=None, help="seconds to wait for the answer")
    parser.add_argument("--config", default="Release", help="launch: the build configuration to run")
    parser.add_argument("--exe", help="launch: the engine executable")
    parser.add_argument("--writable-settings", action="store_true", help="launch: let the engine save its settings")
    options = parser.parse_args(argv)

    try:
        if options.command == "ab":
            # mectl.py ab --json '{"a": {...render values...}, "b": {...}, "frames": 64}'
            spec = json.loads(options.json_args or "{}")
            with EngineClient(options.port) as client:
                result = ab_compare(
                    client,
                    steps_from_spec(spec.get("a")),
                    steps_from_spec(spec.get("b")),
                    frames=int(spec.get("frames", 64)),
                )
            print(result["summary"])
            print(result["folder"])
            return 0
        if options.command == "compare":
            # mectl.py compare A.png B.png
            from image_compare import compare, summary

            first, second = options.pairs
            result = compare(Path(first), Path(second), Path(first).parent / "compare")
            print(summary(result))
            return 0
        if options.command == "launch":
            client = launch(
                engine_args,
                port=options.port,
                exe=options.exe,
                config=options.config,
                read_only_settings=not options.writable_settings,
            )
            result = client.call("ping")
            result["log"] = str(client.log_path)
            client.close()
        else:
            args = json.loads(options.json_args) if options.json_args else {}
            for pair in options.pairs:
                key, separator, value = pair.partition("=")
                if not separator:
                    parser.error(f"'{pair}' is not key=value")
                args[key] = parse_value(value)
            with EngineClient(options.port) as client:
                result = client.call(options.command, timeout=options.timeout, **args)
    except EngineError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except (OSError, RuntimeError, TimeoutError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
