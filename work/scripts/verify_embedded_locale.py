"""Embed and verify the compatible Russian text payload in the signed base APK."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[2]
RELATIVE = Path("bl2_locale/ru_text")


def verify(source, apk=None):
    descriptor = (source / "pack.json").read_bytes()
    pack = json.loads(descriptor)
    header = (ROOT / "arm64_bridge/src/bl2_locale_payload.h").read_text("utf-8")
    assert hashlib.sha256(descriptor).hexdigest().upper() == re.search(
        r'kDescriptorSha256\[\] = "([0-9A-F]+)"', header)[1]
    assert (pack["schema"], pack["package"], pack["version_code"],
            pack["language"], pack["engine_language"]) == (
                2, "com.tt2kgames.Borderlands2", 33, "ru", "INT")
    expected = {"assets/" + str(RELATIVE.as_posix()) + "/pack.json":
                (len(descriptor), hashlib.sha256(descriptor).hexdigest())}
    for item in pack["files"]:
        relative = Path(item["asset"]).relative_to(RELATIVE)
        assert ".." not in relative.parts
        path = source / relative
        assert path.stat().st_size == item["bytes"]
        with path.open("rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == item["sha256"]
        assert f'{{"assets/{item["asset"]}", {item["bytes"]}, "{item["sha256"].upper()}"}}' in header
        expected["assets/" + item["asset"]] = (item["bytes"], item["sha256"])
    assert sum(item["bytes"] for item in pack["files"]) == pack["payload_bytes"]
    if apk:
        with zipfile.ZipFile(apk) as archive:
            names = archive.namelist()
            assert len(names) == len(set(names))
            for name, (size, digest) in expected.items():
                entry = archive.getinfo(name)
                assert entry.compress_type == zipfile.ZIP_STORED and entry.file_size == size
                with archive.open(entry) as stream:
                    assert hashlib.file_digest(stream, "sha256").hexdigest() == digest
    return pack["payload_bytes"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", type=Path)
    parser.add_argument("--apk", type=Path)
    args = parser.parse_args()
    target = ROOT / "app/assets" / RELATIVE
    if args.prepare:
        verify(args.prepare)
        shutil.copytree(args.prepare, target, dirs_exist_ok=True)
    size = verify(target, args.apk)
    print(json.dumps({"status": "PASS", "russian_text_in_base": True,
                      "payload_bytes": size, "apk": str(args.apk) if args.apk else None}))
