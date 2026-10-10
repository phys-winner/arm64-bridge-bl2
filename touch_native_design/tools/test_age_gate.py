"""Extract exact AgeGate sink methods and reuse the bounded metadata fixture.

Compile ui_age_gate_test.cpp with this external output directory on its include
path. Real GfxSession/capture/focus and UiTransactions are compiled normally.
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
for signature in ("void clearAgeGate(", "bool ageGateOwnerCurrent(", "bool ageGateCurrent(", "AgeGateWidgetTarget ageGateHit(",
                  "bool ageGateDown(", "void ageGateMove(", "void ageGateUp("):
    begin = source.index(signature)
    at = source.index("{", begin)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    methods.append(source[begin:end])
args.output.mkdir(parents=True, exist_ok=True)
generated = args.output / "ui_age_gate_sink.inc"
generated.write_text("\n".join(methods), encoding="utf8")
fixture = root / "touch_native_design/tests/ui_model_test.cpp"
(args.output / "ui_age_gate_model_fixture.inc").write_text(
    fixture.read_text(encoding="utf8").split("int main()", 1)[0], encoding="utf8")
(args.output / "ui-age-gate-production-source.json").write_text(json.dumps({
    "source": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest().upper(),
    "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest().upper(),
    "fixture": str(fixture), "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest().upper(),
}, indent=2), encoding="utf8")
print("PASS: extracted original AgeGate phase dispatch and typed metadata fixture")
