"""Verify that the reviewed source cleanup is reflected in compiled DEX class_defs."""

import json
import struct
import sys
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "work/evidence/modern-cleanup-seed145/candidate-files.json"
REQUIRED = {
    "Lcom/tt2kgames/Borderlands2/UE3JavaApp;",
    "Lcom/tt2kgames/Borderlands2/Wrap_NVMouseExtensions;",
    "Lcom/nvidia/nvsi/NvSecureInterfaceUtil;",
    "Lcom/nvidia/gpgs/GoogleServicesSignIn;",
    "Lcom/nvidia/gpgs/SaveGameManager;",
}


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError(f"DEX offset out of bounds: {offset}")
    return struct.unpack_from("<I", data, offset)[0]


def dex_classes(data: bytes) -> set[str]:
    if len(data) < 0x70 or not data.startswith(b"dex\n"):
        raise ValueError("Invalid DEX header")
    string_size, string_off = u32(data, 0x38), u32(data, 0x3C)
    type_size, type_off = u32(data, 0x40), u32(data, 0x44)
    class_size, class_off = u32(data, 0x60), u32(data, 0x64)
    if (string_off + 4 * string_size > len(data)
            or type_off + 4 * type_size > len(data)
            or class_off + 32 * class_size > len(data)):
        raise ValueError("DEX index table exceeds file bounds")

    result = set()
    for index in range(class_size):
        class_type = u32(data, class_off + 32 * index)
        if class_type >= type_size:
            raise ValueError("Class type index exceeds type table")
        descriptor_index = u32(data, type_off + 4 * class_type)
        if descriptor_index >= string_size:
            raise ValueError("Descriptor index exceeds string table")
        string_data = u32(data, string_off + 4 * descriptor_index)
        # Skip the ULEB128 UTF-16 length. Class descriptors here are ASCII.
        for _ in range(5):
            if string_data >= len(data):
                raise ValueError("Truncated DEX string length")
            value = data[string_data]
            string_data += 1
            if value < 0x80:
                break
        else:
            raise ValueError("Invalid DEX string length")
        end = data.index(0, string_data)
        result.add(data[string_data:end].decode("ascii"))
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: verify_removed_classes_in_apk.py <signed.apk>", file=sys.stderr)
        return 2
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    removed = {
        "L" + record["path"].removeprefix("app/smali/").removesuffix(".smali") + ";"
        for record in manifest
    }
    if len(removed) != 524:
        raise ValueError(f"Expected 524 unique cleanup descriptors, got {len(removed)}")

    apk = Path(sys.argv[1])
    classes = set()
    dex_counts = {}
    with zipfile.ZipFile(apk) as archive:
        dex_names = sorted(name for name in archive.namelist()
                           if name.startswith("classes") and name.endswith(".dex"))
        if not dex_names:
            raise ValueError("APK contains no classes*.dex")
        for name in dex_names:
            current = dex_classes(archive.read(name))
            if classes.intersection(current):
                raise ValueError(f"Duplicate class definition in {name}")
            classes.update(current)
            dex_counts[name] = len(current)

    still_defined = sorted(classes.intersection(removed))
    missing_required = sorted(REQUIRED.difference(classes))
    print(json.dumps({"apk": str(apk.resolve()), "dex_classes": dex_counts,
                      "class_definitions": len(classes),
                      "removed_descriptors_checked": len(removed),
                      "removed_still_defined": still_defined,
                      "missing_required": missing_required}, indent=2))
    return 1 if still_defined or missing_required else 0


if __name__ == "__main__":
    sys.exit(main())
