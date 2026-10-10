"""Extract the production drag continuity/completion into an external host fixture.

Compile ui_inventory_test.cpp with the output directory on its include path.
The original GFx session, item capture and cancelDrag are compiled normally.
"""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
path = root / "arm64_bridge/src/bl2_touch_ui_sink.cpp"
source = path.read_text(encoding="utf8")
methods = []
for name in ("InventoryDragSource freshInventorySource(", "void finishInventory("):
    begin = source.index(name)
    at = source.index("{", begin)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    methods.append(source[begin:end])
args.output.mkdir(parents=True, exist_ok=True)
generated = args.output / "ui_inventory_completion.inc"
generated.write_text("\n".join(methods), encoding="utf8")
fixture_path = root / "touch_native_design/tests/ui_model_test.cpp"
(args.output / "ui_inventory_model_fixture.inc").write_text(
    fixture_path.read_text(encoding="utf8").split("int main()", 1)[0], encoding="utf8")
(args.output / "ui-inventory-production-source.json").write_text(json.dumps({
    "source": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest().upper(),
    "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest().upper(),
    "fixture_source": str(fixture_path), "fixture_sha256": hashlib.sha256(fixture_path.read_bytes()).hexdigest().upper(),
}, indent=2), encoding="utf8")
print("PASS: extracted exact native drag continuity/completion and typed Trading metadata fixture")
