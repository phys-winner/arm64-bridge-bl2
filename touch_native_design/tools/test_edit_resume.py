"""Extract the current production resume guard for a host fixture.

Generated files belong to the specified external directory; the project is
read-only. Compile ui_editor_resume_test.cpp with that directory on its include
path, bl2_touch_ui_model.cpp and bl2_touch_ui_transactions.cpp.
"""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source_path = root / "arm64_bridge/src/bl2_touch_widget_sink.cpp"
source = source_path.read_text(encoding="utf8")


def block(needle, semicolon=False):
    begin = source.index(needle)
    at = source.index("{", begin)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end + int(semicolon)]


args.output.mkdir(parents=True, exist_ok=True)
data = block("struct Reader {", True) + "\n" + block("struct Prepared {", True)
data += "\nstruct ResumePredicate {\nexplicit ResumePredicate(DynarmicHost& host):read_(host){}\n"
data += block("struct Observation {", True) + "\n" + block("UiPauseResumeIdentity resumeIdentity(")
data += "\nReader read_;std::function<std::uint64_t()> generation_;Observation preparing_;\n};\n"
generated = args.output / "ui_editor_production.inc"
generated.write_text(data, encoding="utf8")
fixture_path = root / "touch_native_design/tests/ui_model_test.cpp"
fixture = args.output / "ui_editor_model_fixture.inc"
fixture.write_text(fixture_path.read_text(encoding="utf8").split("int main()", 1)[0], encoding="utf8")
(args.output / "ui-editor-production-source.json").write_text(json.dumps({
    "source": str(source_path), "sha256": hashlib.sha256(source_path.read_bytes()).hexdigest().upper(),
    "generated": str(generated), "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest().upper(),
    "fixture_source": str(fixture_path), "fixture_sha256": hashlib.sha256(fixture_path.read_bytes()).hexdigest().upper(),
}, indent=2), encoding="utf8")
print("PASS: extracted exact current Reader/focused resume method; no production callbacks mocked")
