#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# One vector source over several city archives, so a pane can fly between them.
#
# The styles name one source and the app only moves the camera -- "same styles, different
# location" -- but a pmtiles archive is cut to a bbox and answers nothing outside it, and its
# TileJSON's `bounds` is what the cover is clipped to, so a style pointing straight at
# `hongkong_z15.json` cannot be flown to Tokyo however the camera is set.
#
# This serves one TileJSON whose bounds are the world and routes each tile to whichever archive
# holds it. Nothing is copied or re-cut: the archives stay as `pmtiles serve` publishes them and
# this is a few hundred bytes of routing in front.
#
#   ./serve.sh &                     # the archives, on 8080
#   python3 tools/cities.py &        # this, on 8086
#
# Add a city by cutting another archive and naming it below. The cut is the same one the
# tileserver README gives:
#
#   ./pmtiles extract https://build.protomaps.com/YYYYMMDD.pmtiles <name>_z15.pmtiles \
#       --bbox=<w,s,e,n> --maxzoom=15
#
# Low zooms are in every archive -- an extract keeps whole ancestor tiles rather than clipping
# them -- so z0 through about z6 are the same bytes whichever one answers, and the first match
# wins. Above that exactly one archive's bounds contain the tile.
import http.server
import json
import math
import socketserver
import sys
import urllib.error
import urllib.request

UPSTREAM = "http://127.0.0.1:8080"
PORT = 8086

# The archives this routes over, in the order a tie at low zoom is broken.
ARCHIVES = ["hongkong_z15", "tokyo_z15", "sanfrancisco_z15"]


def tile_bounds(z, x, y):
    """The tile's own bbox in degrees, as `min_lon, min_lat, max_lon, max_lat`."""
    n = 2.0**z

    def lon(at):
        return at / n * 360.0 - 180.0

    def lat(at):
        return math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * at / n))))

    return lon(x), lat(y + 1), lon(x + 1), lat(y)


def overlaps(a, b):
    return not (a[2] <= b[0] or a[0] >= b[2] or a[3] <= b[1] or a[1] >= b[3])


def fetch(url, timeout=20):
    """The body, its type, and its *encoding*.

    The third is not optional. `pmtiles serve` hands back the tile as it is stored, which is
    gzipped, and says so in `Content-Encoding`. A proxy that forwards the bytes and drops the
    header is handing a renderer gzip labeled as protobuf, and what it reports is
    "malformed protobuf: wire type 7 at byte 0 is a group" -- a decode error at the first byte,
    naming the tile and not the proxy.
    """
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return (
            response.read(),
            response.headers.get("Content-Type", "application/octet-stream"),
            response.headers.get("Content-Encoding"),
        )


def load_archives():
    """Each archive's bounds and the first one's layer list, from their own TileJSON."""
    held, layers = [], None
    for name in ARCHIVES:
        try:
            body, _, _ = fetch(f"{UPSTREAM}/{name}.json")
        except (urllib.error.URLError, OSError) as why:
            print(f"  {name}: not served by {UPSTREAM} ({why})", file=sys.stderr)
            continue
        meta = json.loads(body)
        held.append((name, meta.get("bounds", [-180, -85, 180, 85])))
        # The vector layer list is a property of the Protomaps schema rather than of the cut, so
        # every archive carries the same one and the first to answer provides it. A style's
        # source needs it: a layer the TileJSON does not declare is one the renderer will not
        # ask a tile for.
        layers = layers or meta.get("vector_layers")
        print(f"  {name}: {meta.get('bounds')}", file=sys.stderr)
    return held, layers


class Router(http.server.BaseHTTPRequestHandler):
    archives: list = []
    layers = None

    def do_GET(self):  # noqa: N802 - the stdlib's spelling
        path = self.path.split("?", 1)[0].lstrip("/")
        if path in ("cities.json", ""):
            return self.send_tilejson()
        parts = path.replace(".mvt", "").split("/")
        if len(parts) != 3 or not all(p.lstrip("-").isdigit() for p in parts):
            return self.send_error(404)
        z, x, y = (int(p) for p in parts)
        box = tile_bounds(z, x, y)
        for name, bounds in self.archives:
            if not overlaps(box, bounds):
                continue
            try:
                body, kind, encoding = fetch(f"{UPSTREAM}/{name}/{z}/{x}/{y}.mvt")
            except (urllib.error.URLError, OSError):
                continue
            self.send_response(200)
            self.send_header("Content-Type", kind)
            if encoding is not None:
                self.send_header("Content-Encoding", encoding)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)
            return None
        # Not an error: a cover reaches past every archive at the edges of a flight, and a 404
        # is how a source says "nothing here" to a renderer that then draws nothing there.
        return self.send_error(404)

    def send_tilejson(self):
        doc = {
            "tilejson": "3.0.0",
            "name": "harbor cities",
            "scheme": "xyz",
            "tiles": [f"http://127.0.0.1:{PORT}/{{z}}/{{x}}/{{y}}.mvt"],
            "minzoom": 0,
            "maxzoom": 15,
            # The world, deliberately. The cover is clipped to this, so bounds that held only one
            # city would stop the camera reaching the others -- which is the whole point of this.
            "bounds": [-180.0, -85.0, 180.0, 85.0],
            "attribution": '<a href="https://www.openstreetmap.org/copyright">&copy; OpenStreetMap</a>',
        }
        if self.layers is not None:
            doc["vector_layers"] = self.layers
        body = json.dumps(doc).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    # Threaded, and with room in the backlog. A tile cover fetches in parallel, and the stdlib's
    # single-threaded default with a backlog of five drops tiles under exactly that -- which
    # reads as a renderer losing them.
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


def main():
    print(f"routing {UPSTREAM} archives on :{PORT}", file=sys.stderr)
    Router.archives, Router.layers = load_archives()
    if not Router.archives:
        print("no archives; is serve.sh running on 8080?", file=sys.stderr)
        return 1
    Server(("127.0.0.1", PORT), Router).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
