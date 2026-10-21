#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# The terrain style's elevation: AWS's open terrarium tiles, clamped at sea level and cached.
#
# `styles/terrain.json` names `http://127.0.0.1:8085/{z}/{x}/{y}.png` and nothing in this tree
# was serving it, so the terrain pane only worked for whoever had already built an archive by
# hand. This is that, written down -- and it answers anywhere rather than one bbox, which is
# what lets the pane fly between cities.
#
#   python3 tools/dem.py &           # this, on 8085
#
# First pass over a city fetches from AWS; everything after that is the cache directory. So the
# demo is offline once it has been somewhere, rather than offline from the start.
#
# # Why the clamp
#
# Terrarium carries bathymetry -- the sea floor runs to -208 m off Hong Kong -- and a DEM whose
# sea floor is modelled makes the shoreline a cliff, whose face the hillshade lights white. The
# clamp puts everything below sea level at exactly zero, which is what the style's color ramp
# and its lowest stop are written against.
import http.server
import os
import socketserver
import sys
import urllib.error
import urllib.request

UPSTREAM = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium"
PORT = 8085
CACHE = os.environ.get("HARBOR_DEM_CACHE", os.path.expanduser("~/.cache/harbor-dem"))

try:
    from PIL import Image
except ImportError:  # pragma: no cover - a dependency note, not a code path
    print("needs Pillow: python3 -m pip install --user Pillow", file=sys.stderr)
    raise


def clamp_to_sea_level(data):
    """Terrarium bytes with everything below zero raised to it.

    `h = red * 256 + green + blue / 256 - 32768`, so zero is exactly `(128, 0, 0)`. Compared on
    the two high channels alone: blue is the fractional part and cannot carry a sample below
    zero on its own.
    """
    image = Image.open(data).convert("RGB")
    pixels = image.load()
    width, height = image.size
    for y in range(height):
        for x in range(width):
            red, green, blue = pixels[x, y]
            if red < 128 or (red == 128 and green == 0 and blue == 0):
                if red * 256 + green + blue / 256 - 32768 < 0:
                    pixels[x, y] = (128, 0, 0)
    return image


def tile(z, x, y):
    """The clamped tile's bytes, from the cache or from AWS."""
    path = os.path.join(CACHE, str(z), str(x), f"{y}.png")
    if os.path.isfile(path):
        with open(path, "rb") as held:
            return held.read()
    with urllib.request.urlopen(f"{UPSTREAM}/{z}/{x}/{y}.png", timeout=30) as response:
        raw = response.read()
    import io

    image = clamp_to_sea_level(io.BytesIO(raw))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # Written beside and renamed, so a cover fetching the same tile twice at once cannot read a
    # half-written file -- which decodes as a torn DEM and draws as a cliff.
    staging = f"{path}.{os.getpid()}"
    image.save(staging, "PNG")
    os.replace(staging, path)
    with open(path, "rb") as held:
        return held.read()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802 - the stdlib's spelling
        parts = self.path.split("?", 1)[0].lstrip("/").replace(".png", "").split("/")
        if len(parts) != 3 or not all(p.isdigit() for p in parts):
            return self.send_error(404)
        try:
            body = tile(*(int(p) for p in parts))
        except (urllib.error.URLError, OSError, ValueError):
            # A DEM tile that is not there is a gap in the ground, not a failure: the renderer
            # draws the layers that do not need it and leaves that square unshaded.
            return self.send_error(404)
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)
        return None

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    # Threaded, with room in the backlog: a terrain cover fetches tens of tiles at once and the
    # stdlib's single-threaded default drops them, which the renderer reports as tiles that
    # failed to build.
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


def main():
    os.makedirs(CACHE, exist_ok=True)
    print(f"terrarium, clamped, cached in {CACHE}, on :{PORT}", file=sys.stderr)
    Server(("127.0.0.1", PORT), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
