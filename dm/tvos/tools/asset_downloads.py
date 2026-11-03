"""Attach public Gofile file IDs to immutable asset manifests."""

import json
from pathlib import PurePosixPath
from uuid import UUID


def add_gofile_downloads(groups, records_path):
    records = json.loads(records_path.read_text())
    if not isinstance(records, list):
        raise ValueError("Gofile upload records must be an array")
    uploads = {(item["name"], item["sha256"]): item for item in records}
    for group in groups:
        for chunk in group["chunks"]:
            name = chunk["name"]
            upload = uploads.get((group["source"] + "/" + name, chunk["sha256"]))
            if upload is None or upload.get("bytes") != chunk["bytes"]:
                raise ValueError("No matching Gofile upload for " + name)
            cloud = upload["cloud"]
            if (PurePosixPath(name).name != name or cloud.get("name") != name
                    or cloud.get("size") != chunk["bytes"]):
                raise ValueError("Gofile upload metadata does not match " + name)
            # Storage servers migrate files. Resolve their current URL on the
            # device using a separate anonymous reader, never the upload token.
            chunk["gofile_id"] = str(UUID(cloud["id"]))
