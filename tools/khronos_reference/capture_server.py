#!/usr/bin/env python3
"""Serves the Khronos glTF Sample Viewer on http://127.0.0.1:8765/viewer/ and writes the canvas
captures it posts back to assets/khronos/captures/viewer/<name>.png. See README.md.

The viewer's files are proxied from its release site and kept in memory: served from the same
origin as /save, the page can post its captures, which a browser may refuse from the public site
to a local address. Models and environments still come from raw.githubusercontent.com."""

import mimetypes
import re
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from common import CAPTURES

PORT = 8765
OUTPUT = CAPTURES / "viewer"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
VIEWER_SITE = "https://github.khronos.org/glTF-Sample-Viewer-Release/"
# Injected at the top of the viewer's page. The viewer's WebGL context clears its drawing buffer
# once a frame is shown, which reads back as black; preserving it lets captureViewer read the last
# frame at any time. captureViewer(name, width, height) scales the canvas to the engine capture's
# size and posts it to /save; it refuses a canvas of another aspect ratio, which would stretch.
CAPTURE_SCRIPT = b"""<script>
(function () {
  const getContext = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (type, attributes) {
    if (type === "webgl2" || type === "webgl") {
      attributes = Object.assign({}, attributes, { preserveDrawingBuffer: true });
    }
    return getContext.call(this, type, attributes);
  };
  window.captureViewer = async function (name, width = 667, height = 541) {
    // The viewer sizes its canvas (and refits its camera to the new aspect) on a resize event, and
    // an emulated viewport size may not have sent one: resize until the backing store matches.
    const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
    const source = document.querySelector("canvas");
    const sized = () => source.width === Math.round(source.clientWidth * devicePixelRatio);
    for (let attempt = 0; attempt < 20 && !sized(); ++attempt) {
      window.dispatchEvent(new Event("resize"));
      await sleep(500);
    }
    if (!sized()) {
      throw new Error("the canvas never took its size: " + source.width + "x" + source.height);
    }
    await sleep(1000);
    const aspect = source.clientWidth / source.clientHeight;
    if (Math.abs(aspect - width / height) > 0.01) {
      throw new Error("canvas is " + source.clientWidth + "x" + source.clientHeight + ", not the aspect of " + width + "x" + height);
    }
    const target = document.createElement("canvas");
    target.width = width;
    target.height = height;
    const context = target.getContext("2d");
    context.imageSmoothingQuality = "high";
    context.drawImage(source, 0, 0, width, height);
    const blob = await new Promise((resolve) => target.toBlob(resolve, "image/png"));
    const response = await fetch("/save?name=" + encodeURIComponent(name), { method: "POST", body: blob });
    return response.status + ": " + name + " " + blob.size + " bytes from " + source.width + "x" + source.height;
  };
})();
</script>"""
_viewer_cache = {}
_viewer_lock = threading.Lock()


def viewer_file(relative):
    """The viewer's file at this path, from memory or the release site; None when it has none."""
    with _viewer_lock:
        if relative in _viewer_cache:
            return _viewer_cache[relative]
    try:
        with urllib.request.urlopen(VIEWER_SITE + relative) as response:
            body = response.read()
            content_type = response.headers.get("Content-Type") or mimetypes.guess_type(relative)[0]
    except urllib.error.HTTPError:
        return None
    with _viewer_lock:
        _viewer_cache[relative] = (body, content_type or "application/octet-stream")
        return _viewer_cache[relative]


class Handler(BaseHTTPRequestHandler):
    def cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Private-Network", "true")

    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path
        # /viewer-debug/ is the same viewer with its "Transmission Factor" debug output showing the
        # raw refracted sample instead (f_specular_transmission), for comparing the transmission
        # lookup itself.
        prefix = next((p for p in ("/viewer/", "/viewer-debug/") if path.startswith(p)), None)
        if prefix is None or ".." in path:
            self.send_response(404)
            self.end_headers()
            return
        relative = path[len(prefix):] or "index.html"
        found = viewer_file(relative)
        if found is None:
            self.send_response(404)
            self.end_headers()
            return
        body, content_type = found
        if relative == "index.html":
            body = body.replace(b"<head>", b"<head>" + CAPTURE_SCRIPT, 1)
        if prefix == "/viewer-debug/" and relative == "GltfSVApp.js":
            body = body.replace(
                b"g_finalColor.rgb=vec3(materialInfo.transmissionFactor);",
                b"g_finalColor.rgb=f_specular_transmission;",
                1)
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        name = parse_qs(urlparse(self.path).query).get("name", [""])[0]
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or not body.startswith(PNG_SIGNATURE):
            self.send_response(400)
            self.cors()
            self.end_headers()
            return
        OUTPUT.mkdir(parents=True, exist_ok=True)
        (OUTPUT / f"{name}.png").write_bytes(body)
        print(f"saved {name}.png ({len(body) / 1e3:.0f} kB)", flush=True)
        self.send_response(200)
        self.cors()
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    print(f"Sample Viewer at http://127.0.0.1:{PORT}/viewer/, captures to {OUTPUT}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
