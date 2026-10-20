#!/usr/bin/env python3
"""Offline Vulkan 1.1 conversion of the exact shipped, normalized GLSL catalogue.

Outputs remain separate from the accepted GLES assets. A failed stage is recorded
with its exact source and diagnostics; a complete manifest requires every stage.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NDK_TOOLS = Path('C:/Users/Xilonen/AppData/Local/Android/Sdk/ndk/27.2.12479018/shader-tools/windows-x86_64')
CATALOGUE_SHA = 'd26172dac51eb373d7590ab76ea7b5bae59b74cbb72684f62c64e3f99d37ce91'
RULES = 'bl2-vulkan-1:std140-private-stage-parameters;explicit-sampler-bindings;gl-depth-to-vk;original-image-row-order'
LOCATIONS = (0, 1, 2, 3, 4, 5, 6, 7, 12, 13)
CONSTANTS = re.compile(r'^(?:uniform|UNIFORM_SPECIFIER) (vec4|ivec4|bool) ([VP]Const(?:Float|Int|Bool|Global|Bones))\[(\d+)\];$', re.M)
SAMPLERS = re.compile(r'^uniform (sampler\w+|SHAD__SMP) ([VP])Sampler(\d+);$', re.M)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def read_catalogue(path):
    if file_sha(path) != CATALOGUE_SHA:
        raise ValueError('The shipped catalogue identity changed; review the input before baking')
    with path.open('rb') as f:
        header = f.read(56)
        if header[:8] != b'BL2SSO1\0':
            raise ValueError('Bad stage table magic')
        version, count, size = struct.unpack_from('<IIQ', header, 8)
        if version != 1 or count != 38980 or size + 56 != path.stat().st_size:
            raise ValueError('Bad stage table header')
        previous = None
        payload = hashlib.sha256()
        for _ in range(count):
            record = f.read(40)
            if len(record) != 40:
                raise ValueError('Truncated stage record')
            target, n = struct.unpack_from('<II', record)
            if target not in (0x8b30, 0x8b31) or not 0 < n <= 131072:
                raise ValueError('Invalid stage target/length')
            source = f.read(n)
            key = target, record[8:40]
            if len(source) != n or hashlib.sha256(source).digest() != key[1] or b'\0' in source:
                raise ValueError('Stage source identity failed')
            if previous is not None and key <= previous:
                raise ValueError('Unsorted or duplicate stage')
            previous = key
            payload.update(record + source)
            yield target, key[1].hex(), source.decode('latin1')
        if f.read(1) or payload.digest() != header[24:56]:
            raise ValueError('Stage table payload identity failed')


def adapt(source, vertex):
    prefix = 'V' if vertex else 'P'
    if not source.startswith('#version 320 es\n') or '\0' in source or len(source) > 131072:
        raise ValueError('Unrecognized normalized GLSL')
    for name in ('ENABLE_UNIFORM_BUFFER_OBJECTS', 'ENABLE_MULTIDRAW_INDIRECT'):
        if source.count('#define ' + name + ' 0\n') != 1:
            raise ValueError('Unrecognized active uniform/multidraw path')
    constants = list(CONSTANTS.finditer(source))
    if not constants or any(m[2][0] != prefix for m in constants):
        raise ValueError('Missing or wrong-stage parameters')
    fields, offset, declarations, names = [], 0, [], set()
    for m in constants:
        glsl_type, name, n = m[1], m[2], int(m[3])
        if name in names or not 0 < n <= 4096:
            raise ValueError('Duplicate/oversized parameter field')
        names.add(name)
        fields.append(dict(name=name, type=glsl_type, count=n, offset=offset, stride=16))
        offset += n * 16
        declarations.append(f'    {glsl_type} {name}[{n}];')
    if offset > 65536:
        raise ValueError('Parameters exceed the measured uniform-buffer limit')
    source = CONSTANTS.sub('', source)
    samplers = []
    def sampler(m):
        glsl_type, stage, unit = m[1], m[2], int(m[3])
        if stage != prefix or not 0 <= unit < 16:
            raise ValueError('Wrong-stage/out-of-range sampler')
        if glsl_type == 'SHAD__SMP':
            glsl_type = 'sampler2DShadow'
        if glsl_type not in ('sampler2D', 'samplerCube', 'sampler2DShadow'):
            raise ValueError('Unsupported sampler: ' + glsl_type)
        binding = 2 + unit + (0 if vertex else 16)
        samplers.append(dict(unit=unit, type=glsl_type, set=0, binding=binding))
        return f'layout(set=0, binding={binding}) uniform {glsl_type} {prefix}Sampler{unit};'
    source = SAMPLERS.sub(sampler, source)
    if len({s['unit'] for s in samplers}) != len(samplers):
        raise ValueError('Duplicate sampler unit')
    direction = 'out' if vertex else 'in'
    declared = {int(m[1]) for m in re.finditer(rf'^layout\(location=(\d+)\) {direction} vec4 UnTexcoord\d+;$', source, re.M)}
    if declared - set(LOCATIONS):
        raise ValueError('Unrecognized stage interface location')
    extra = ''.join(f'layout(location={n}) {direction} vec4 UnTexcoord{n};\n' for n in LOCATIONS if n not in declared)
    binding = 0 if vertex else 1
    block = f'layout(std140, set=0, binding={binding}) uniform Bl2{prefix}Parameters {{\n' + '\n'.join(declarations) + '\n};\n'
    if len(re.findall(r'^void main\(', source, re.M)) != 1:
        raise ValueError('Missing or duplicate main entry point')
    source = source.replace('#version 320 es\n', '#version 450\n', 1)
    source = re.sub(r'^void main\(', extra + block + ('void Bl2LegacyMain(' if vertex else 'void main('), source, count=1, flags=re.M)
    if vertex:
        # Keep texture row coordinates unchanged; Android presentation must flip
        # the final image, so offscreen and uploaded textures share GL semantics.
        source += '\nvoid main() { Bl2LegacyMain(); gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; }\n'
    else:
        if source.count('out vec4 FragmentColor;') != 1:
            raise ValueError('Unrecognized fragment output')
        source = source.replace('out vec4 FragmentColor;', 'layout(location=0) out vec4 FragmentColor;', 1)
    layout = dict(parameter_binding=binding, parameter_bytes=offset, fields=fields, samplers=samplers,
                  attribute_locations=[int(n) for n in re.findall(r'^layout\(location=(\d+)\) in vec4 _Un_Attr\d+;', source, re.M)] if vertex else [])
    return source, layout


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def bake(output, catalogue, workers):
    output.mkdir(parents=True, exist_ok=False)
    stages = output / 'stages'
    stages.mkdir()
    compiler, validator = NDK_TOOLS / 'glslc.exe', NDK_TOOLS / 'spirv-val.exe'
    tools = dict(compiler_sha256=file_sha(compiler), validator_sha256=file_sha(validator),
                 compiler_version=subprocess.check_output([str(compiler), '--version'], text=True).strip(),
                 validator_version=subprocess.check_output([str(validator), '--version'], text=True).strip())
    # Validate the full input before launching any compiler or committing output.
    catalogue_rows = list(read_catalogue(catalogue))
    results = []
    def compile_stage(row):
        target, digest, source = row
        name = re.search(r'//ShaderFileName:([^\n]*)', source)[1].strip()
        stem = ('vert-' if target == 0x8b31 else 'frag-') + digest
        glsl, spirv = stages / (stem + '.glsl'), stages / (stem + '.spv')
        result = dict(target=target, normalized_sha256=digest, shader_name=name)
        try:
            converted, layout = adapt(source, target == 0x8b31)
            glsl.write_bytes(converted.encode('latin1'))
            command = [str(compiler), '--target-env=vulkan1.1', '-fshader-stage=' + ('vert' if target == 0x8b31 else 'frag'),
                       '-O', str(glsl), '-o', str(spirv)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if compiled.returncode:
                raise ValueError(compiled.stdout + compiled.stderr)
            validated = subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(spirv)], capture_output=True, text=True, timeout=30)
            if validated.returncode:
                raise ValueError(validated.stdout + validated.stderr)
            data = spirv.read_bytes()
            if len(data) < 20 or len(data) % 4 or struct.unpack_from('<I', data)[0] != 0x07230203:
                raise ValueError('Invalid compiled SPIR-V container')
            result.update(status='PASS', vulkan_glsl_sha256=sha(converted.encode('latin1')), spirv_sha256=sha(data),
                          spirv_bytes=len(data), spirv_file=str(spirv.relative_to(output)), layout=layout)
            glsl.unlink()
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            result.update(status='FAIL', error=str(error))
            if not glsl.exists():
                glsl.write_bytes(source.encode('latin1'))
        return result
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for result in pool.map(compile_stage, catalogue_rows):
            results.append(result)
            if len(results) % 500 == 0:
                progress = dict(completed=len(results), total=len(catalogue_rows), status_counts=dict(Counter(r['status'] for r in results)))
                write_json(output / 'progress.json', progress)
                print(json.dumps(progress), flush=True)
    manifest = dict(status='COMPLETE' if all(r['status'] == 'PASS' for r in results) else 'INCOMPLETE',
                    catalogue_sha256=CATALOGUE_SHA, rules=RULES, rules_sha256=sha(RULES.encode()), tools=tools,
                    counts=dict(Counter(r['status'] for r in results)), stages=results,
                    hardware_rendering_verified=False, game_backend_integrated=False)
    write_json(output / 'manifest.json', manifest)
    print(json.dumps({k:v for k,v in manifest.items() if k != 'stages'}, indent=2))
    return manifest['status'] == 'COMPLETE'


def self_check():
    prefix = '#version 320 es\n#define ENABLE_UNIFORM_BUFFER_OBJECTS 0\n#define ENABLE_MULTIDRAW_INDIRECT 0\n'
    frag = prefix + 'UNIFORM_SPECIFIER vec4 PConstFloat[3];\nuniform bool PConstBool[2];\nuniform sampler2D PSampler1;\nout vec4 FragmentColor;\nvoid main() { FragmentColor=PConstFloat[1]; }\n'
    converted, layout = adapt(frag, False)
    assert 'binding=19' in converted and 'location=0) out vec4 FragmentColor' in converted
    assert layout['fields'][1]['offset'] == 48 and layout['fields'][1]['stride'] == 16 and layout['parameter_bytes'] == 80
    vert = prefix + 'UNIFORM_SPECIFIER vec4 VConstFloat[2];\nvoid main() { gl_Position=VConstFloat[0]; }\n'
    converted, layout = adapt(vert, True)
    assert 'void Bl2LegacyMain()' in converted and 'gl_Position.z + gl_Position.w' in converted and layout['parameter_binding'] == 0
    for broken in (frag.replace('PSampler1', 'PSampler16'), frag.replace('PConstFloat', 'VConstFloat'), frag + 'void main() {}\n', frag.replace('OBJECTS 0', 'OBJECTS 1')):
        try:
            adapt(broken, False)
        except ValueError:
            pass
        else:
            raise AssertionError('Malformed stage accepted')
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'manifest.json'
        write_json(path, dict(value=3))
        assert json.loads(path.read_text()) == dict(value=3) and not path.with_suffix('.json.tmp').exists()
    print('PASS: sampler bindings, private stage layouts, std140 bool stride, depth mapping, malformed stages and atomic manifest')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--catalogue', type=Path, default=ROOT / 'app/assets/bl2_shader_stages.bin')
    parser.add_argument('--workers', type=int, default=6)
    parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        self_check()
        return
    if not args.output or args.output.exists() or not 1 <= args.workers <= 16:
        parser.error('Provide a new output directory and 1..16 workers')
    if not bake(args.output, args.catalogue, args.workers):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
