"""Extract the exact native Status Inventory capture/completion for a host check.

Compile ui_status_inventory_test.cpp with this output directory on its include
path. GFx sessions, typed metadata, item capture and gestures compile normally.
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
for name in ("bool inventoryDiagnosticEnabled(", "void inventoryDiagnostic(",
             "InventoryDragFamily inventoryFamily(", "InventoryWidgetTarget captureInventory(", "bool inventoryDown(",
             "bool statusInventoryItemCurrent(", "InventoryDragSource freshInventorySource(", "void finishInventory(", "void inventoryUpdate("):
    begin = source.index(name)
    at = source.index("{", begin)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    methods.append(source[begin:end])
args.output.mkdir(parents=True, exist_ok=True)
generated = args.output / "ui_status_inventory_consumer.inc"
generated.write_text("\n".join(methods), encoding="utf8")
fixture_path = root / "touch_native_design/tests/ui_model_test.cpp"
fixture = fixture_path.read_text(encoding="utf8").split("struct TradingFixture", 1)[0]
# Both real readers share one rebased image in the combined consumer fixture.
# Only the reused fixture's synthetic base changes; production methods do not.
fixture = fixture.replace("static constexpr std::uint32_t base = 0x10000;",
                          "static constexpr std::uint32_t base = 0x10000000;", 1)
(args.output / "ui_status_inventory_model_fixture.inc").write_text(
    fixture + "}\n", encoding="utf8")
transport_path = root / "touch_native_design/tests/ui_inventory_test.cpp"
transport = transport_path.read_text(encoding="utf8")
begin = transport.index("struct InventoryVm final")
transport = transport[begin:transport.index("struct InventoryConsumer", begin)].replace(
    "InventoryVm final", "InventoryVmBase").replace("InventoryVm(", "InventoryVmBase(")
(args.output / "ui_status_inventory_transport.inc").write_text(transport, encoding="utf8")
(args.output / "status-inventory-extracted-source.json").write_text(json.dumps({
    "source": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest().upper(),
    "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest().upper(),
    "fixture_source": str(fixture_path), "fixture_sha256": hashlib.sha256(fixture_path.read_bytes()).hexdigest().upper(),
    "transport_source": str(transport_path), "transport_sha256": hashlib.sha256(transport_path.read_bytes()).hexdigest().upper(),
}, indent=2), encoding="utf8")
print("PASS: exact Status Inventory capture/completion and shared typed/GFx fixture extracted")
