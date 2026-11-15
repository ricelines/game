"""Exercise actual Godot HTTP import with a delayed real baked GLB."""

import argparse
import http.server
import json
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asset", type=Path)
    options = parser.parse_args()
    asset = options.asset.resolve()
    assert asset.is_relative_to(ROOT / "assets")
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            name = self.path.rsplit("/", 1)[-1]
            if name not in {"manifest.json", "lod0.glb", "lod1.glb", "lod2.glb"}:
                self.send_error(404)
                return
            # The real manifest arrives first; the render proxy must persist until import.
            time.sleep(0.3 if name == "manifest.json" else 1)
            body = (asset / name).read_bytes()
            requests.append({"file": name, "bytes": len(body)})
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_arguments):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        result = subprocess.run(
            [
                str(ROOT / ".tools/bin/godot"),
                "--headless",
                "--path",
                str(ROOT / "client"),
                "--script",
                "res://tests/proxy_test.gd",
                "--",
                f"http://127.0.0.1:{server.server_port}",
                asset.name,
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        print(result.stdout)
        print(result.stderr)
        print(json.dumps({"actual_http_requests": requests}))
        raise SystemExit(result.returncode)
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


if __name__ == "__main__":
    main()
