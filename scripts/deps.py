#!/usr/bin/env python3
"""Fetch pinned, SHA-256 verified archives; no submodules or pip dependencies."""
import hashlib
from pathlib import Path
import tarfile
import urllib.request
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (
    ("tinyusb-0.17.0", "hathach/tinyusb", "0.17.0", "tinyusb-0.17.0.tar.gz",
     "666632c92d900dba1a74a14884355b2e252814d7ade5b72bcde3726108d92b06"),
    ("nrfx-2.11.0", "NordicSemiconductor/nrfx", "v2.11.0", "nrfx-v2.11.0.tar.gz",
     "2f8132db721d8a6d2560cf285c34c3a7a91f68e36d2df9c8734b83e4a6e3b0a5"),
    ("CMSIS_5-5.9.0", "ARM-software/CMSIS_5", "5.9.0", "cmsis-5.9.0.tar.gz",
     "9ce693dd15c41f02506adda79323c077912b3211303ca0d9b28b8ebe735a7031"),
)

def main():
    if sys.version_info < (3, 12):
        raise SystemExit("Python 3.12+ is required for safe dependency archive extraction")
    cache, deps = ROOT / ".cache", ROOT / "deps"
    cache.mkdir(exist_ok=True)
    deps.mkdir(exist_ok=True)
    for directory, repo, tag, archive_name, digest in PACKAGES:
        marker = deps / directory / ".verified"
        if marker.is_file() and marker.read_text().strip() == digest:
            continue
        archive = cache / archive_name
        if not archive.exists():
            print(f"Downloading {repo} {tag}", flush=True)
            tmp = archive.with_suffix(".part")
            urllib.request.urlretrieve(f"https://codeload.github.com/{repo}/tar.gz/refs/tags/{tag}", tmp)
            tmp.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise SystemExit(f"Checksum mismatch: {archive}; remove it and retry")
        with tarfile.open(archive) as tar:
            # Explicit filter also rejects escaping links / paths on Python 3.12+.
            tar.extractall(deps, filter="data")
        marker.write_text(digest + "\n")
        print(f"Verified {directory}")

if __name__ == "__main__":
    main()
