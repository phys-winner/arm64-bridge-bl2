"""Extract the production stopped-chat episode guard into an external fixture."""
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
begin = source.index("void stopChatEpisode(")
at = source.index("{", begin)
depth, end = 1, at + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
args.output.mkdir(parents=True, exist_ok=True)
generated = args.output / "ui_chat_episode_production.inc"
generated.write_text(source[begin:end], encoding="utf8")
fixture_path = root / "touch_native_design/tests/ui_model_test.cpp"
fixture = args.output / "ui_chat_model_fixture.inc"
fixture.write_text(fixture_path.read_text(encoding="utf8").split("int main()", 1)[0], encoding="utf8")
(args.output / "ui-chat-episode-production-source.json").write_text(json.dumps({
    "source": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest().upper(),
    "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest().upper(),
    "fixture_source": str(fixture_path), "fixture_sha256": hashlib.sha256(fixture_path.read_bytes()).hexdigest().upper(),
}, indent=2), encoding="utf8")
print("PASS: extracted exact current stopped-chat episode guard")
