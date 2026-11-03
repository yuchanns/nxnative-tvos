#!/usr/bin/env python3
"""Check real R2 publication semantics using a dedicated validation Worker."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import secrets
import struct
from urllib.error import HTTPError
from urllib.request import Request, urlopen


def archive():
    data = secrets.token_bytes(128)
    manifest = json.dumps(
        {"version": 1, "entries": [{"path": "validation.sav", "type": "file",
          "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}]},
        separators=(",", ":"), sort_keys=True,
    ).encode()
    payload = b"NXSAVE01" + struct.pack("<I", len(manifest)) + manifest + data
    return hashlib.sha256(payload).hexdigest(), payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--token-file", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    if not args.url.startswith("https://"):
        parser.error("A remote HTTPS validation Worker is required")
    token = args.token_file.read_text().strip()
    results = []

    def request(path, method="GET", data=None, parent=None, authorized=True):
        headers = {"Authorization": "Bearer " + (token if authorized else "invalid"),
                   "User-Agent": "nxnative/1.0"}
        if parent is not None:
            headers["X-Save-Parent"] = parent
        if data is not None:
            headers["Content-Type"] = "application/octet-stream"
        req = Request(args.url.rstrip("/") + path, data=data, method=method, headers=headers)
        try:
            with urlopen(req, timeout=60) as response:
                return response.status, response.read(8193)
        except HTTPError as error:
            return error.code, error.read(8193)

    def check(name, passed):
        results.append({"check": name, "passed": bool(passed)})
        if not passed:
            raise RuntimeError(name)

    try:
        code, body = request("/v1/info")
        info = json.loads(body)
        check("dedicated_validation_scope", code == 200 and info.get("scope") == "validation-cloud-save")
        code, _ = request("/v1/latest", authorized=False)
        check("unauthorized_request", code == 401)
        code, body = request("/v1/latest")
        check("initial_head_readable", code in (200, 404))
        parent = json.loads(body)["revision"] if code == 200 else "none"
        first, payload = archive()
        code, body = request("/v1/snapshots/" + first, "PUT", payload, parent)
        check("complete_snapshot_published", code == 201 and json.loads(body)["revision"] == first)
        commit = json.loads(body)["commit"]
        code, body = request("/v1/snapshots/" + first, "PUT", payload, parent)
        check("retry_is_idempotent", code == 200 and json.loads(body)["commit"] == commit)
        code, body = request("/v1/snapshots/" + first)
        check("download_matches_archive", code == 200 and body == payload)
        bad_sha, bad_payload = archive()
        code, _ = request("/v1/snapshots/" + bad_sha, "PUT", bad_payload[:-1] + bytes([bad_payload[-1] ^ 1]), first)
        check("bad_checksum_rejected", code >= 400)
        code, body = request("/v1/latest")
        check("failed_upload_keeps_previous_head", code == 200 and json.loads(body)["revision"] == first)
        contenders = [archive(), archive()]
        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(lambda item: request("/v1/snapshots/" + item[0], "PUT", item[1], first), contenders))
        check("exactly_one_concurrent_writer_wins", sorted(code for code, _ in outcomes) == [201, 409])
        winner = next(item[0] for item, result in zip(contenders, outcomes) if result[0] == 201)
        stale_sha, stale_payload = archive()
        code, _ = request("/v1/snapshots/" + stale_sha, "PUT", stale_payload, first)
        check("stale_parent_rejected", code == 409)
        code, body = request("/v1/latest")
        check("winning_snapshot_remains_current", code == 200 and json.loads(body)["revision"] == winner)
        code, body = request("/v1/snapshots/" + first)
        check("previous_complete_version_retained", code == 200 and body == payload)
    finally:
        args.report.write_text(json.dumps({"endpoint": args.url, "checks": results}, indent=2) + "\n")
    print(f"Passed {len(results)} remote checks in the validation scope.")


if __name__ == "__main__":
    main()
