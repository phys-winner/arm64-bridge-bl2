"""Extract the exact native FName lookup and footer key dispatch for host checks.

Compile ui_footer_key_test.cpp with this output directory on its include path.
The fixture models original GNames/FName storage and InputKey's six arguments.
"""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source_path = root / "arm64_bridge/src/bl2_touch_ui_sink.cpp"
source = source_path.read_text(encoding="utf8")
methods = []
for signature in ("bool array(", "std::string name(", "bool keyName(", "bool footerKey("):
    begin = source.index(signature)
    at = source.index("{", begin)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    methods.append(source[begin:end])
args.output.mkdir(parents=True, exist_ok=True)
generated = args.output / "ui_footer_keys.inc"
generated.write_text("\n".join(methods), encoding="utf8")
(args.output / "ui-footer-production-source.json").write_text(json.dumps({
    "source": str(source_path),
    "sha256": hashlib.sha256(source_path.read_bytes()).hexdigest().upper(),
    "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest().upper(),
    "methods": ["array", "name", "keyName", "footerKey"],
}, indent=2), encoding="utf8")
print("PASS: extracted exact production FName storage guards and footer key dispatch")
