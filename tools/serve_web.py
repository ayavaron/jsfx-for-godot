#!/usr/bin/env python3
"""Serve a Godot Web export with the headers needed for threaded Wasm.

Usage:
    python tools/serve_web.py --dir build/web_export_threads --port 8061
"""

import argparse
import http.server
import mimetypes
import os
import socketserver
from functools import partial


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", default=".")
    parser.add_argument("--port", type=int, default=8060)
    args = parser.parse_args()

    mimetypes.add_type("application/wasm", ".wasm")
    mimetypes.add_type("application/octet-stream", ".pck")
    mimetypes.add_type("text/javascript", ".js")

    directory = os.path.abspath(args.dir)
    handler = partial(Handler, directory=directory)
    with Server(("127.0.0.1", args.port), handler) as httpd:
        print(f"serving {directory} on http://127.0.0.1:{args.port}", flush=True)
        httpd.serve_forever()


if __name__ == "__main__":
    main()
