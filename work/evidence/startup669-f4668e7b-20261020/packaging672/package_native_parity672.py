"""Package version33 BaseOnly universal APK from native672 and a source-matching bridge; root-run only."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from elftools.elf.elffile import ELFFile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
JAVA = Path('C:/Portable/jdk-24.0.1/bin/java.exe')
TOOLS = ROOT.parent / 'tools/android'
ANDROID = TOOLS / 'sdk/build-tools/37.0.0'
SIGNER = Path('C:/Portable/Total.Commander/Utilites/Just Another Tool for APK/bin')
CERT = 'a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc'
GUEST = 'a0b06ef59f4c89c0889654196951d2deb966cfeac7180699b5724f0e26938814'
VOICE_SPLIT = 'dee6a9f040fda7d80ad6221acec2e526213a50fa78e17399ab0ece5ed0122717'
NS = 'http://schemas.android.com/apk/res/android'
NATIVE_LIBS = {'lib/armeabi-v7a/' + n for n in ['libUnrealEngine3.so', 'libUE3Runtime.so', 'libtsec_wrapper.so']}
BRIDGE_LIB = 'lib/arm64-v8a/libUnrealEngine3.so'
GUEST_ASSET = 'assets/guest_libs/libUnrealEngine3.so'
RUNTIME_ENTRY = 'lib/armeabi-v7a/libUE3Runtime.so'
SHARED_RUNTIME_PATH = RUNTIME_ENTRY.encode('ascii')
IDA_EXCLUSIONS = {GUEST_ASSET + '.' + suffix for suffix in ['id0', 'id1', 'id2', 'nam', 'til', 'gpr']}

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def file_pin(path):
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest(path))

def run(argv, label):
    process = subprocess.run([str(x) for x in argv], capture_output=True, text=True,
        encoding='utf-8', errors='replace')
    text = process.stdout + process.stderr
    (HERE / ('universal-' + label + '.log')).write_text(text, 'utf-8')
    assert process.returncode == 0, (label, process.returncode, text[-4000:])
    return text

def signed(apk, label):
    result = run([JAVA, '-jar', SIGNER / 'apksigner.jar', 'verify', '--verbose', '--print-certs', apk], label)
    assert re.search(r'certificate SHA-256 digest:\s*' + CERT, result, re.I)
    for scheme in ['v2', 'v3']:
        assert re.search(r'Verified using ' + scheme + r' scheme.*:\s*true', result, re.I)

def classes(archive):
    sys.path.insert(0, str(ROOT / 'work/scripts'))
    from verify_removed_classes_in_apk import dex_classes
    seen, result = set(), {}
    for name in sorted(n for n in archive.namelist() if re.fullmatch(r'classes\d*\.dex', n)):
        current = dex_classes(archive.read(name))
        assert not seen.intersection(current), name
        seen.update(current)
        result[name] = dict(count=len(current), sha256=hashlib.sha256(archive.read(name)).hexdigest())
    assert result
    return seen, result

def abis_from_badging(badging):
    return {abi for line in badging.splitlines()
        if line.startswith(('native-code:', 'alt-native-code:'))
        for abi in re.findall(r"'([^']+)'", line)}

def version33_config(config):
    assert re.findall(rb'^  versionCode: (33)\r?$', config, re.M) == [b'33']
    assert re.findall(rb'^  versionName: 1\.0\.0\.0\.(33)\r?$', config, re.M) == [b'33']
    assert len(re.findall(rb'^  versionCode:', config, re.M)) == 1
    assert len(re.findall(rb'^  versionName:', config, re.M)) == 1
    return config


def common_payloads(archive):
    result = {}
    for name in archive.namelist():
        if name != 'AndroidManifest.xml' and not name.startswith(('lib/', 'META-INF/', 'assets/guest_libs/')):
            with archive.open(name) as stream:
                result[name] = (archive.getinfo(name).file_size, hashlib.file_digest(stream, 'sha256').hexdigest())
    return result


def assert_final_inventory(names):
    assert len(names) == len(set(names)), 'Duplicate ZIP entry'
    assert {p for p in names if p.startswith('lib/')} == NATIVE_LIBS | {BRIDGE_LIB}
    assert not any(p.startswith('assets/guest_libs/') for p in names), 'Guest asset copy is forbidden'
    assert not any(p.endswith('.obb') or p.startswith('assets/bl2_cache/') for p in names)
    assert not any(p.endswith(('.id0', '.id1', '.id2', '.nam', '.til', '.gpr')) for p in names)


def main(args):
    native, bridge, source_stage, output = [p.resolve(strict=True) for p in
        [args.native, args.bridge, args.source_stage, args.output.parent]]
    output = output / args.output.name
    assert output not in [native, bridge] and output.suffix == '.apk'
    stage = HERE / 'universal-stage'
    unsigned, aligned = HERE / 'universal-unsigned.apk', HERE / 'universal-aligned.apk'
    assert not (HERE / 'SHARED_RUNTIME_BUILD.json').exists()
    if args.resume_verification:
        assert stage.is_dir() and all(p.is_file() for p in [output, unsigned, aligned])
        existing_output = file_pin(output)
    else:
        assert not any(p.exists() for p in [output, stage, unsigned, aligned])
        existing_output = None
    receipt_paths = [args.native_receipt.resolve(strict=True), args.bridge_receipt.resolve(strict=True)]
    receipt_pins = [file_pin(p) for p in receipt_paths]
    native_receipt, bridge_receipt = [json.loads(p.read_text('utf-8-sig')) for p in receipt_paths]
    before = [file_pin(native), file_pin(bridge)]
    for receipt, pin, seed in zip([native_receipt, bridge_receipt], before, [672, args.bridge_seed]):
        assert receipt['seed'] == seed and receipt['sha256'].lower() == pin['sha256']
        assert receipt['base_only'] is True and receipt['dlc_content_enabled'] is False
        assert receipt['qa_console_compiled'] is False and receipt['debug_menu_default_enabled'] is False
    assert native_receipt['logging'] is False and native_receipt['native_performance_probe'] is False
    assert bridge_receipt['final_build'] is True and bridge_receipt['silent_logcat'] is True
    native_cmake = ROOT / 'arm32_compat/CMakeLists.txt'
    native_cmake_pin = file_pin(native_cmake)
    assert 'jni_bridge.cpp' not in native_cmake.read_text('utf-8')
    source_expected = {}
    for receipt, groups in [(native_receipt, ['source', 'shared_source']), (bridge_receipt,
        ['source_hashes', 'bridge_patch_hashes', 'bridge_build_support_hashes',
         'touch_core_source_hashes', 'android_source_hashes', 'app_hook_hashes'])]:
        for group in groups:
            for entry in receipt[group]:
                path, expected = Path(entry['Path']).resolve(strict=True), entry['Hash'].lower()
                assert path not in source_expected or source_expected[path] == expected, path
                source_expected[path] = expected
    source_pins = [file_pin(p) for p in sorted(source_expected)]
    for pin in source_pins:
        assert pin['sha256'] == source_expected[Path(pin['path'])], pin['path']
    caches = [ROOT / 'work/build/native-arm32/CMakeCache.txt',
              ROOT / 'work/build' / bridge_receipt['native_build_dir'] / 'CMakeCache.txt']
    for path in caches:
        assert re.search(r'^BL2_ENABLE_DLC_CONTENT:BOOL=OFF\r?$', path.read_text('utf-8'), re.M), path
    cache_pins = [file_pin(p) for p in caches]
    source_config_pin = file_pin(source_stage / 'apktool.yml')
    config = (source_stage / 'apktool.yml').read_bytes()
    final_config = version33_config(config)
    signed(native, 'native-signature')
    signed(bridge, 'bridge-signature')
    voice = args.voice_split.resolve(strict=True)
    voice_pin = file_pin(voice)
    assert voice_pin['sha256'] == VOICE_SPLIT, 'The exact user-requested Russian voice split is required'
    signed(voice, 'ru-voice-signature')
    voice_badging = run([ANDROID / 'aapt2.exe', 'dump', 'badging', voice], 'ru-voice-badging')
    assert "name='com.tt2kgames.Borderlands2'" in voice_badging
    assert "versionCode='33'" in voice_badging and "versionName='1.0.0.0.33'" in voice_badging
    assert "split='ru_voice'" in voice_badging
    with zipfile.ZipFile(voice) as z:
        names = z.namelist()
        assert len(names) == len(set(names))
        assert not any(n.startswith('lib/') or re.fullmatch(r'classes\d*\.dex', n) for n in names)
    with zipfile.ZipFile(native) as n, zipfile.ZipFile(bridge) as b:
        for archive, abi in [(n, 'armeabi-v7a'), (b, 'arm64-v8a')]:
            assert len(archive.namelist()) == len(set(archive.namelist()))
            assert {p.split('/')[1] for p in archive.namelist() if p.startswith('lib/') and p.endswith('.so')} == {abi}
            assert not any(p.endswith('.obb') or p.startswith('assets/bl2_cache/') for p in archive.namelist())
        assert {p for p in n.namelist() if p.startswith('lib/')} == NATIVE_LIBS
        assert {p for p in b.namelist() if p.startswith('lib/')} == {BRIDGE_LIB}
        assert not any(p.startswith('assets/guest_libs/') for p in n.namelist())
        assert {p for p in b.namelist() if p.startswith('assets/guest_libs/')} <= IDA_EXCLUSIONS | {GUEST_ASSET}
        payload = {p: n.read(p) for p in NATIVE_LIBS}
        payload[BRIDGE_LIB] = b.read(BRIDGE_LIB)
        donor_guest = b.read(GUEST_ASSET)
        assert hashlib.sha256(donor_guest).hexdigest() == GUEST
        assert donor_guest == payload[RUNTIME_ENTRY]
        assert bridge_receipt['guest_sha256'].lower() == GUEST
        assert SHARED_RUNTIME_PATH in payload[BRIDGE_LIB], 'Bridge672 lacks the shared runtime ZIP path'
        expected_common = common_payloads(n)
        assert expected_common == common_payloads(b), 'Donors contain different Java/resource/shared bytes'
        native_classes, native_dex = classes(n)
        bridge_classes, bridge_dex = classes(b)
        assert native_classes == bridge_classes and native_dex == bridge_dex
        donor_guest_pin = dict(path=GUEST_ASSET, bytes=len(donor_guest), sha256=GUEST)
    if not args.resume_verification:
        # Only this derivative is changed. Resume never enters this build/sign path.
        def ignore(directory, names):
            relative = Path(directory).relative_to(source_stage).as_posix()
            if relative == '.':
                return [n for n in names if n in ['build', 'dist', 'lib']]
            if relative == 'assets':
                return [n for n in names if n == 'guest_libs']
            return []
        shutil.copytree(source_stage, stage, ignore=ignore)
        (stage / 'apktool.yml').write_bytes(final_config)
        manifest = stage / 'AndroidManifest.xml'
        ET.register_namespace('android', NS)
        tree = ET.parse(manifest)
        assert tree.getroot().attrib['package'] == 'com.tt2kgames.Borderlands2'
        apps = tree.getroot().findall('application')
        assert len(apps) == 1
        for key in ['multiArch', 'use32bitAbi', 'extractNativeLibs']:
            apps[0].set('{' + NS + '}' + key, 'true')
        tree.write(manifest, encoding='utf-8', xml_declaration=True)
        for name, blob in payload.items():
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            assert not path.exists(), name
            path.write_bytes(blob)
        assert not (stage / 'assets/guest_libs').exists()
        assert (stage / 'apktool.yml').read_bytes() == final_config
        run([JAVA, '-jar', TOOLS / 'apktool.jar', 'b', '-f', stage,
            '-p', ROOT / 'work/tool-cache/apktool-framework', '-o', unsigned], 'apktool')
        run([ANDROID / 'zipalign.exe', '-P', '16', '-f', '4', unsigned, aligned], 'align')
        run([JAVA, '-jar', SIGNER / 'apksigner.jar', 'sign', '--key', SIGNER / 'testkey.pk8',
            '--cert', SIGNER / 'testkey.x509.pem', '--v1-signing-enabled', 'true',
            '--v2-signing-enabled', 'true', '--v3-signing-enabled', 'true', '--out', output, aligned], 'sign')
    signed(output, 'output-signature')
    run([ANDROID / 'zipalign.exe', '-c', '-P', '16', '4', output], 'check-align')
    badging = run([ANDROID / 'aapt2.exe', 'dump', 'badging', output], 'badging')
    assert "versionCode='33'" in badging and "versionName='1.0.0.0.33'" in badging
    assert "minSdkVersion:'24'" in badging and "targetSdkVersion:'37'" in badging
    assert abis_from_badging(badging) == {'armeabi-v7a', 'arm64-v8a'}
    xml = run([ANDROID / 'aapt2.exe', 'dump', 'xmltree', output, '--file', 'AndroidManifest.xml'], 'manifest')
    for key in ['multiArch', 'use32bitAbi', 'extractNativeLibs']:
        assert re.search(re.escape(key) + r'\([^\n]*?\)=true\b', xml), key
    assert not re.search(r'debuggable\([^\n]*?\)=true\b', xml), 'Final release cannot be debuggable'
    with zipfile.ZipFile(output) as z:
        assert_final_inventory(z.namelist())
        assert common_payloads(z) == expected_common
        runtime_copies = [p for p in z.namelist() if z.getinfo(p).file_size == len(payload[RUNTIME_ENTRY])
            and hashlib.sha256(z.read(p)).hexdigest() == GUEST]
        assert runtime_copies == [RUNTIME_ENTRY], runtime_copies
        assert classes(z)[0] == native_classes and classes(z)[1] == native_dex
        for name, blob in payload.items():
            entry = z.getinfo(name)
            assert entry.compress_type == zipfile.ZIP_STORED and z.read(name) == blob
            with output.open('rb') as stream:
                stream.seek(entry.header_offset)
                local = stream.read(30)
            assert local[:4] == b'PK\x03\x04'
            data_offset = entry.header_offset + 30 + int.from_bytes(local[26:28], 'little') + int.from_bytes(local[28:30], 'little')
            assert data_offset % 16384 == 0, (name, data_offset)
            if name in NATIVE_LIBS | {BRIDGE_LIB} and not name.endswith('/libUE3Runtime.so'):
                elf = ELFFile(io.BytesIO(blob))
                assert all(s['p_align'] >= 16384 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD')
    run([sys.executable, '-B', '-X', 'utf8', ROOT / 'work/scripts/verify_embedded_locale.py', '--apk', output], 'locale')
    # The DEX bytes/classes above match both donors; the old cleanup ledger is not a release input.
    assert before == [file_pin(native), file_pin(bridge)]
    assert receipt_pins == [file_pin(p) for p in receipt_paths]
    assert voice_pin == file_pin(voice)
    assert cache_pins == [file_pin(p) for p in caches]
    assert native_cmake_pin == file_pin(native_cmake)
    bridge_cmake = ROOT / 'arm64_bridge/CMakeLists.txt'
    assert digest(bridge_cmake) == bridge_receipt['cmake_source_sha256'].lower(), 'Bridge CMake changed after donor build'
    assert source_config_pin == file_pin(source_stage / 'apktool.yml')
    assert source_pins == [file_pin(p) for p in sorted(source_expected)]
    output_pin = file_pin(output)
    if args.resume_verification:
        assert output_pin == existing_output, 'Read-only verification changed the signed APK'
    result = dict(status='UNIVERSAL33_SINGLE_SHARED_RUNTIME_HOST_PACKAGING_PASS_DEVICE_PENDING',
        inputs=before, output=output_pin, base_only=True, dlc_content_enabled=False,
        multiArch=True, use32bitAbi=True, extractNativeLibs=True, version_code=33, version_name='1.0.0.0.33', certificate_sha256=CERT,
        payload_sha256={n: hashlib.sha256(b).hexdigest() for n, b in payload.items()},
        dex=native_dex, shared_payloads=len(expected_common), guest_runtime=RUNTIME_ENTRY, guest_runtime_copies=1,
        guest_assets_in_output=0, donor_guest_asset=donor_guest_pin, native_source_exclusions=[],
        russian_voice_split=voice_pin, russian_voice_split_host_compatibility=True,
        native_cmake=native_cmake_pin, exact_IDA_exclusions=sorted(IDA_EXCLUSIONS),
        source_stage=str(source_stage), source_config=source_config_pin,
        staged_config_sha256=hashlib.sha256(final_config).hexdigest(),
        cmake_caches=cache_pins, input_receipts=receipt_pins, source_hashes=source_pins,
        verification_mode='resumed_existing_signed_apk' if args.resume_verification else 'fresh_packaging',
        build_steps_executed=not args.resume_verification,
        resumed_signed_APK_unchanged=output_pin == existing_output if args.resume_verification else None,
        canonical_or_input_APKs_or_device_modified=False,
        runtime_limit='Actual primaryCpuAbi/process and retained-save upgrade checks remain root-owned; --abi override alone is not proof for a multiArch package.')
    with (HERE / 'SHARED_RUNTIME_BUILD.json').open('x', encoding='utf-8', newline='\n') as receipt:
        receipt.write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge-seed', type=int, choices=[672], default=672)
    parser.add_argument('--voice-split', type=Path, default=Path('D:/ai_test/old_game_restoration/borderlands_2/pub/split_ru_voice.apk'))
    parser.add_argument('--native', type=Path, default=ROOT / 'work/build/bl2-arm32-native-seed672.apk')
    parser.add_argument('--bridge', type=Path, default=ROOT / 'work/build/bl2-arm64-bridge-seed672.apk')
    parser.add_argument('--native-receipt', type=Path, default=ROOT / 'work/build/arm32-seed672-build.json')
    parser.add_argument('--bridge-receipt', type=Path, default=ROOT / 'work/build/seed672-build.json')
    parser.add_argument('--source-stage', type=Path, default=ROOT / 'work/build/native-arm32-app')
    parser.add_argument('--resume-verification', action='store_true',
        help='Verify the existing signed APK without rebuilding, aligning or signing it again')
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args())
