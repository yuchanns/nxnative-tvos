"""Verify bounded parallel downloads using a local HTTP server and real archives."""
import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from test_assets import fragment


@contextmanager
def server(payloads, indexes, failures=None, slow=False):
    lock = threading.Lock()
    state = {"active": 0, "peak": 0, "window": 0, "requests": Counter()}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            name = self.path.removeprefix("/")
            with lock:
                state["active"] += 1
                state["peak"] = max(state["peak"], state["active"])
                state["requests"][name] += 1
                attempt = state["requests"][name]
                missing = sum(not (indexes / f"{item}.json").exists() for item in state["requests"])
                state["window"] = max(state["window"], missing)
            try:
                body = payloads[name]
                failure = (failures or {}).get(name) if attempt == 1 else None
                if failure == "http":
                    time.sleep(0.1)
                    self.send_error(503)
                    return
                if failure == "checksum":
                    body = body[:-1] + bytes([body[-1] ^ 1])
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body[:1024])
                self.wfile.flush()
                time.sleep(3 if slow and name != "000.nxassets" else 0.3)
                self.wfile.write(body[1024:])
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with lock:
                    state["active"] -= 1

        def log_message(self, *_):
            pass

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{httpd.server_port}", state
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join()


def fixture(base, count=12):
    pieces = [bytes([i]) * 262144 for i in range(count)]
    content = b"".join(pieces)
    payloads, jobs = {}, []
    for i, piece in enumerate(pieces):
        name = f"{i:03}.nxassets"
        payloads[name] = b"NXROMFS2" + fragment("large.cpk", len(content), i * len(piece), piece)
        jobs.append({"chunk": {"name": name, "sha256": hashlib.sha256(payloads[name]).hexdigest(),
                               "bytes": len(payloads[name]), "files": 1},
                     "root": str(base / "romfs"), "index": str(base / "asset-index" / f"{name}.json")})
    return content, payloads, jobs


def run(binary, base, jobs, endpoint, success):
    manifest = base / "jobs.json"
    manifest.write_text(json.dumps(jobs))
    result = subprocess.run([binary, str(manifest), str(base), endpoint], capture_output=True, timeout=35)
    assert (result.returncode == 0) == success, result.stderr.decode()
    summary = json.loads(result.stdout)
    assert summary["success"] == success
    assert summary["peak_writers"] <= 1, summary
    assert summary["peak_archives"] <= 4, summary
    assert not list((base / "temp").glob("*"))
    snapshots = [s for s in summary["samples"] if "downloaded_bytes" in s]
    assert all(0 <= s["active_downloads"] <= 4 for s in snapshots)
    assert all(0 <= s["downloaded_bytes"] <= s["total_download_bytes"] for s in snapshots)
    assert all(s["fraction"] < 1 for s in snapshots)
    done = [s["completed_chunks"] for s in snapshots]
    assert done == sorted(done)
    return summary


def main(binary):
    with tempfile.TemporaryDirectory(prefix="nxnative-downloads-") as temporary:
        base = Path(temporary)
        content, payloads, jobs = fixture(base)
        failures = {"002.nxassets": "checksum", "004.nxassets": "http"}
        with server(payloads, base / "asset-index", failures) as (endpoint, state):
            summary = run(binary, base, jobs, endpoint, True)
            assert state["peak"] == 4, state
            assert state["window"] <= 4, state
            assert summary["completed"] == len(jobs) and summary["receipts_valid"]
            assert max(s.get("active_downloads", 0) for s in summary["samples"]) == 4
            assert all(state["requests"][name] == (2 if name in failures else 1) for name in payloads)
            assert (base / "romfs" / "large.cpk").read_bytes() == content
            before = state["requests"].copy()
            run(binary, base, jobs, endpoint, True)
            assert state["requests"] == before, "Completed receipts must avoid network requests"
            # Repair one missing receipt without touching the other file fragments.
            (base / "asset-index" / "005.nxassets.json").unlink()
            run(binary, base, jobs, endpoint, True)
            assert state["requests"] - before == Counter({"005.nxassets": 1})
            assert (base / "romfs" / "large.cpk").read_bytes() == content
    with tempfile.TemporaryDirectory(prefix="nxnative-download-failure-") as temporary:
        base = Path(temporary)
        content, payloads, jobs = fixture(base)
        original = payloads["000.nxassets"]
        # A valid outer checksum with a corrupt inner fragment must fail unpack,
        # cancel siblings, clean temporary files, and never start later chunks.
        payloads["000.nxassets"] = original[:-1] + bytes([original[-1] ^ 1])
        jobs[0]["chunk"]["sha256"] = hashlib.sha256(payloads["000.nxassets"]).hexdigest()
        with server(payloads, base / "asset-index", slow=True) as (endpoint, state):
            start = time.monotonic()
            run(binary, base, jobs, endpoint, False)
            assert time.monotonic() - start < 2.5, "In-flight requests were not cancelled promptly"
            assert len(state["requests"]) <= 4 and state["window"] <= 4, state
        payloads["000.nxassets"] = original
        jobs[0]["chunk"]["sha256"] = hashlib.sha256(original).hexdigest()
        with server(payloads, base / "asset-index") as (endpoint, state):
            summary = run(binary, base, jobs, endpoint, True)
            assert summary["completed"] == len(jobs) and summary["receipts_valid"]
            assert (base / "romfs" / "large.cpk").read_bytes() == content
    print("Parallel download, bounded spool, serial writes, checksum/HTTP retry, cancellation, and resume passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    main(str(Path(parser.parse_args().binary).resolve()))
