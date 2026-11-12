#!/usr/bin/env python3
"""Fetch the pinned, checksum-verified HarfBuzz source into the build cache."""
from pathlib import Path
import hashlib
import io
import tarfile
import urllib.request

VERSION = "12.2.0"
SHA256 = "ecb603aa426a8b24665718667bda64a84c1504db7454ee4cadbd362eea64e545"
URL = f"https://github.com/harfbuzz/harfbuzz/releases/download/{VERSION}/harfbuzz-{VERSION}.tar.xz"
cache = Path(__file__).resolve().parents[1] / "build_gen"
cache.mkdir(exist_ok=True)
archive = cache / f"harfbuzz-{VERSION}.tar.xz"
if not archive.exists():
    request = urllib.request.Request(URL, headers={"User-Agent": "Sonix-local-build"})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SystemExit("HarfBuzz source checksum mismatch")
    archive.write_bytes(data)
data = archive.read_bytes()
if hashlib.sha256(data).hexdigest() != SHA256:
    raise SystemExit("HarfBuzz source checksum mismatch")
with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as bundle:
    for member in bundle.getmembers():
        path = Path(member.name)
        if path.is_absolute() or ".." in path.parts or path.parts[0] != f"harfbuzz-{VERSION}":
            raise SystemExit("Unexpected HarfBuzz archive path")
        if member.issym() or member.islnk():
            raise SystemExit("Unexpected HarfBuzz archive link")
    bundle.extractall(cache)

# Archive timestamps precede this script; mark the completed dependency current.
(cache / f"harfbuzz-{VERSION}" / "src" / "hb.h").touch()
