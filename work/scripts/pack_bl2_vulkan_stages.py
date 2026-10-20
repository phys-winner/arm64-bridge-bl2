#!/usr/bin/env python3
"""Audit exact original GLSL identities and atomically pack every baked Vulkan stage."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import tempfile

import bake_bl2_shader_stages as original
import bake_bl2_vulkan_stages as vulkan

ROOT = Path(__file__).resolve().parents[2]
MAGIC, HEADER_BYTES, RAW_BYTES, INDEX_BYTES = b'BL2VSP1\0', 256, 40, 80
MAX_RAW, MAX_STAGES, MAX_SPIRV, MAX_PACK = 100000, 50000, 1024 * 1024, 1024 * 1024 * 1024
BANKS = ('VConstGlobal', 'VConstFloat', 'VConstBones', 'VConstInt', 'VConstBool',
         'PConstGlobal', 'PConstFloat', 'PConstInt', 'PConstBool')
TYPES = {'vec4': 1, 'ivec4': 2, 'bool': 3}
SAMPLER_TYPES = {'sampler2D': 1, 'samplerCube': 2, 'sampler2DShadow': 3}
GL_TO_ARB = {0x8b31: 0x8620, 0x8b30: 0x8804}


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def identities():
    return dict(normalizer_sha256=vulkan.file_sha(Path(original.__file__)),
                converter_sha256=vulkan.file_sha(Path(vulkan.__file__)),
                packer_sha256=vulkan.file_sha(Path(__file__)), rules=vulkan.RULES,
                rules_sha256=vulkan.sha(vulkan.RULES.encode()))


def audit(patch, catalogue, output):
    if output.exists():
        raise ValueError('audit output already exists')
    expected = {(GL_TO_ARB[t], digest) for t, digest, _ in vulkan.read_catalogue(catalogue)}
    mappings, seen_normalized, occurrences = {}, set(), 0
    for target, raw in original.records(patch):
        if target not in GL_TO_ARB or not 0 < len(raw) <= original.MAX_STAGE or b'\0' in raw:
            raise ValueError('invalid extracted raw GLSL')
        arb = GL_TO_ARB[target]
        raw_sha, normalized_sha = vulkan.sha(raw), vulkan.sha(original.normalize(raw))
        key, value = (arb, raw_sha), (normalized_sha, len(raw))
        if (arb, normalized_sha) not in expected:
            raise ValueError(f'raw source has no shipped normalized stage: {arb:x}:{raw_sha}')
        if key in mappings and mappings[key] != value:
            raise ValueError('raw identity collision')
        mappings[key] = value
        seen_normalized.add((arb, normalized_sha))
        occurrences += 1
        if len(mappings) > MAX_RAW:
            raise ValueError('raw mapping count exceeds the bounded ARM32 index')
    missing = sorted(expected - seen_normalized)
    if missing:
        raise ValueError(f'{len(missing)} normalized stages have no exact original mapping: {missing[:8]}')
    rows = [dict(target=t, raw_sha256=d, normalized_sha256=n, raw_bytes=size)
            for (t, d), (n, size) in sorted(mappings.items())]
    value = dict(status='COMPLETE', catalogue_sha256=vulkan.CATALOGUE_SHA,
                 original_patch_sha256=vulkan.file_sha(patch), original_patch_bytes=patch.stat().st_size,
                 source_identity=identities(), original_occurrences=occurrences,
                 unique_raw_stages=len(rows), normalized_stages=len(expected), missing_normalized=0,
                 raw_map_sha256=vulkan.sha(canonical(rows)), stages=rows,
                 hardware_rendering_verified=False)
    vulkan.write_json(output, value)
    print(json.dumps({k: v for k, v in value.items() if k != 'stages'}, indent=2))


def layout_bytes(layout, target):
    vertex = target == 0x8b31
    if target not in GL_TO_ARB or layout['parameter_binding'] != (0 if vertex else 1):
        raise ValueError('wrong-stage uniform binding')
    fields, samplers, attributes, offset, groups = [], [], [], 0, set()
    for f in layout['fields']:
        group = BANKS.index(f['name'])
        kind, count = TYPES[f['type']], f['count']
        if (group in groups or (group < 5) != vertex or
                kind != (2 if group in (3, 7) else 3 if group in (4, 8) else 1) or
                not isinstance(count, int) or not 0 < count <= 4096 or
                f['offset'] != offset or f['stride'] != 16):
            raise ValueError('invalid field identity, count, type or independent std140 offset')
        fields.append(struct.pack('<IIII', group, kind, count, offset))
        groups.add(group)
        offset += count * 16
    if not 0 < len(fields) <= (5 if vertex else 4) or not 0 < offset <= 65536 or layout['parameter_bytes'] != offset:
        raise ValueError('invalid uniform range')
    units = set()
    for s in layout['samplers']:
        unit, kind = s['unit'], SAMPLER_TYPES[s['type']]
        if not isinstance(unit, int) or not 0 <= unit < 16 or unit in units or s['set'] != 0 or s['binding'] != unit + (2 if vertex else 18):
            raise ValueError('invalid sampler resource binding')
        units.add(unit)
        samplers.append(struct.pack('<III', unit, kind, s['binding']))
    locations = layout['attribute_locations']
    if len(set(locations)) != len(locations) or len(locations) > 16 or (not vertex and locations) or any(not isinstance(n, int) or not 0 <= n < 16 for n in locations):
        raise ValueError('invalid vertex attribute locations')
    attributes = [struct.pack('<I', n) for n in locations]
    body = b''.join(fields + samplers + attributes)
    return (struct.pack('<IIIIIIII', 1, len(fields), len(samplers), len(attributes),
                        0 if vertex else 1, offset, 0, 32 + len(body)) + body)


def reflect(data, layout, target):
    """Verify optimized SPIR-V resource decorations against its source layout."""
    if not 20 <= len(data) <= MAX_SPIRV or len(data) % 4:
        raise ValueError('invalid SPIR-V bytes')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203 or not 0x10000 <= words[1] <= 0x10300 or not 0 < words[3] <= 1048576 or words[4]:
        raise ValueError('invalid Vulkan 1.1 SPIR-V header')
    types, constants, variables, decorations, members, entries = {}, {}, {}, {}, {}, []
    at = 5
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 65535
        if not count or count > len(words) - at:
            raise ValueError('truncated SPIR-V instruction')
        args = words[at+1:at+count]
        if op == 15:  # OpEntryPoint
            if len(args) < 3:
                raise ValueError('invalid SPIR-V entry point')
            entries.append(args[0])
        elif 20 <= op <= 32 and args:
            types[args[0]] = (op, args[1:])
        elif op == 43 and len(args) == 3:
            constants[args[1]] = args[2]
        elif op == 59 and len(args) >= 3:
            variables[args[1]] = (args[0], args[2])
        elif op == 71 and len(args) >= 2:
            key = args[0], args[1]
            if key in decorations:
                raise ValueError('duplicate SPIR-V decoration')
            decorations[key] = args[2:]
        elif op == 72 and len(args) >= 3:
            key = args[0], args[1], args[2]
            if key in members:
                raise ValueError('duplicate SPIR-V member decoration')
            members[key] = args[3:]
        at += count
    if entries != [0 if target == 0x8b31 else 4]:
        raise ValueError('SPIR-V execution model differs from exact stage target')
    declared_samplers = {s['binding']: s for s in layout['samplers']}
    bindings, active_samplers, active_attributes = set(), set(), set()
    for var, (ptr, storage) in variables.items():
        if target == 0x8b31 and storage == 1 and (var, 30) in decorations:
            location = decorations[var, 30]
            if len(location) != 1 or location[0] not in layout['attribute_locations'] or location[0] in active_attributes:
                raise ValueError('SPIR-V vertex input location differs from exact source layout')
            active_attributes.add(location[0])
        if storage not in (0, 2):  # UniformConstant or Uniform
            continue
        descriptor_set, binding = decorations.get((var, 34)), decorations.get((var, 33))
        if descriptor_set != (0,) or not binding or len(binding) != 1 or binding[0] in bindings:
            raise ValueError('missing/duplicate resource descriptor set or binding')
        binding = binding[0]
        bindings.add(binding)
        pointer_op, pointer_args = types[ptr]
        if pointer_op != 32 or pointer_args[0] != storage:
            raise ValueError('invalid SPIR-V resource pointer')
        resource_op, resource_args = types[pointer_args[1]]
        if storage == 2:
            struct_id = pointer_args[1]
            if binding != layout['parameter_binding'] or resource_op != 30 or decorations.get((struct_id, 2)) != () or len(resource_args) != len(layout['fields']):
                raise ValueError('SPIR-V private parameter block differs from layout')
            for index, (array_id, field) in enumerate(zip(resource_args, layout['fields'])):
                array_op, array_args = types[array_id]
                if (array_op != 28 or len(array_args) != 2 or constants.get(array_args[1]) != field['count'] or
                        decorations.get((array_id, 6)) != (16,) or members.get((struct_id, index, 35)) != (field['offset'],)):
                    raise ValueError('SPIR-V std140 array count/stride/offset differs from layout')
                element_op, element_args = types[array_args[0]]
                if field['type'] == 'bool':
                    valid = element_op == 21 and element_args == (32, 0)
                else:
                    scalar_op, scalar_args = types[element_args[0]] if element_op == 23 else (0, ())
                    valid = element_op == 23 and element_args[1:] == (4,) and ((scalar_op, scalar_args) == (22, (32,)) if field['type'] == 'vec4' else (scalar_op, scalar_args) == (21, (32, 1)))
                if not valid:
                    raise ValueError('SPIR-V parameter element type differs from layout')
        else:
            sampler = declared_samplers.get(binding)
            if sampler is None or resource_op != 27 or len(resource_args) != 1:
                raise ValueError('SPIR-V uses an undeclared sampler')
            image_op, image_args = types[resource_args[0]]
            dimension = 3 if sampler['type'] == 'samplerCube' else 1
            depth = 1 if sampler['type'] == 'sampler2DShadow' else 0
            if image_op != 25 or len(image_args) != 7 or image_args[1:6] != (dimension, depth, 0, 0, 1):
                raise ValueError('SPIR-V sampler dimensions/comparison differ from layout')
            active_samplers.add(binding)
    return dict(active_sampler_bindings=sorted(active_samplers), active_attribute_locations=sorted(active_attributes))


def pack(manifest_path, raw_path, patch, catalogue, output):
    if output.exists():
        raise ValueError('pack output already exists')
    manifest_bytes, raw_bytes = manifest_path.read_bytes(), raw_path.read_bytes()
    manifest, raw = json.loads(manifest_bytes), json.loads(raw_bytes)
    identity = identities()
    if (manifest['status'] != 'COMPLETE' or raw['status'] != 'COMPLETE' or
            manifest['catalogue_sha256'] != vulkan.CATALOGUE_SHA or raw['catalogue_sha256'] != vulkan.CATALOGUE_SHA or
            manifest['rules'] != vulkan.RULES or manifest['rules_sha256'] != identity['rules_sha256'] or
            raw['source_identity']['normalizer_sha256'] != identity['normalizer_sha256'] or
            raw['source_identity']['converter_sha256'] != identity['converter_sha256'] or
            raw['original_patch_bytes'] != patch.stat().st_size or raw['original_patch_sha256'] != vulkan.file_sha(patch) or
            raw['raw_map_sha256'] != vulkan.sha(canonical(raw['stages'])) or
            raw['missing_normalized'] or raw['normalized_stages'] != 38980):
        raise ValueError('incomplete or stale source/manifest/raw-map identity')
    for name in ('compiler', 'validator'):
        executable = vulkan.NDK_TOOLS / ('glslc.exe' if name == 'compiler' else 'spirv-val.exe')
        if manifest['tools'][name + '_sha256'] != vulkan.file_sha(executable):
            raise ValueError('baked tool identity changed')
    stages = manifest['stages']
    if (len(stages) != 38980 or len(stages) > MAX_STAGES or manifest['counts'] != {'PASS': len(stages)} or
            any(s['status'] != 'PASS' for s in stages)):
        raise ValueError('all 38980 stages must have valid SPIR-V; no omissions or substitutions')
    stages = sorted(stages, key=lambda s: (s['target'], s['normalized_sha256']))
    keys = [(s['target'], s['normalized_sha256']) for s in stages]
    if len(set(keys)) != len(keys):
        raise ValueError('duplicate normalized target/digest')
    stage_indices = {(GL_TO_ARB[t], d): i for i, (t, d) in enumerate(keys)}
    raw_rows = raw['stages']
    if not 0 < len(raw_rows) <= MAX_RAW or len(raw_rows) != raw['unique_raw_stages']:
        raise ValueError('invalid raw mapping count')
    raw_keys = [(r['target'], r['raw_sha256']) for r in raw_rows]
    if raw_keys != sorted(set(raw_keys)) or len({(r['target'], r['normalized_sha256']) for r in raw_rows}) != len(stages):
        raise ValueError('duplicate/unsorted or incomplete original source mappings')
    raw_index = b''.join(struct.pack('<I', t) + bytes.fromhex(d) + struct.pack('<I', stage_indices[(t, r['normalized_sha256'])])
                         for (t, d), r in zip(raw_keys, raw_rows))
    index_size = len(raw_index) + len(stages) * INDEX_BYTES
    data_start = HEADER_BYTES + index_size
    normalized_rows = vulkan.read_catalogue(catalogue)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.', delete=False) as file:
        temporary = Path(file.name)
        try:
            file.write(b'\0' * data_start)
            stage_index, total_spirv = [], 0
            for number, (stage, normalized) in enumerate(zip(stages, normalized_rows, strict=True)):
                target, digest, source = normalized
                if (target, digest) != keys[number]:
                    raise ValueError('manifest stage identity differs from shipped catalogue')
                converted, layout = vulkan.adapt(source, target == 0x8b31)
                if layout != stage['layout'] or vulkan.sha(converted.encode('latin1')) != stage['vulkan_glsl_sha256']:
                    raise ValueError('manifest layout/transformation differs from canonical converter')
                relative = Path(stage['spirv_file'])
                path = (manifest_path.parent / relative).resolve()
                if relative.is_absolute() or manifest_path.parent.resolve() not in path.parents:
                    raise ValueError('SPIR-V path escapes baked manifest')
                data = path.read_bytes()
                if stage['spirv_bytes'] != len(data) or stage['spirv_sha256'] != vulkan.sha(data):
                    raise ValueError('baked SPIR-V payload identity failed')
                # The original bake's attribute regex misses names such as _Un_AttrPosition0.
                # Reconcile inputs from exact source before reflecting the optimized variables.
                reflected_layout = dict(layout, attribute_locations=[int(n) for n in vulkan.re.findall(
                    r'^layout\(location=(\d+)\) in vec4 _Un_Attr[A-Za-z0-9_]+;$', source, vulkan.re.M)] if target == 0x8b31 else [])
                reflection = reflect(data, reflected_layout, target)
                # Runtime binds only optimized live descriptors; no SPIR-V reflection occurs on the phone.
                active_layout = dict(layout, samplers=[s for s in layout['samplers'] if s['binding'] in reflection['active_sampler_bindings']],
                                     attribute_locations=reflection['active_attribute_locations'])
                block = bytearray(layout_bytes(active_layout, target))
                struct.pack_into('<I', block, 24, len(data))
                payload = block + data
                offset = file.tell()
                if offset + len(payload) > MAX_PACK:
                    raise ValueError('pack exceeds bounded random-access size')
                stage_index.append(struct.pack('<IIQ', target, len(payload), offset) + bytes.fromhex(digest) + hashlib.sha256(payload).digest())
                file.write(payload)
                total_spirv += len(data)
                if (number + 1) % 5000 == 0:
                    print(json.dumps(dict(packed=number+1, total=len(stages))), flush=True)
            total = file.tell()
            index = raw_index + b''.join(stage_index)
            header = (MAGIC + struct.pack('<IIIIQQQQ', 1, HEADER_BYTES, len(raw_rows), len(stages), total,
                                          HEADER_BYTES, HEADER_BYTES + len(raw_index), data_start) +
                      bytes.fromhex(vulkan.CATALOGUE_SHA) + bytes.fromhex(identity['rules_sha256']) +
                      hashlib.sha256(canonical(dict(tools=manifest['tools'], sources=identity))).digest() +
                      bytes.fromhex(raw['raw_map_sha256']) + hashlib.sha256(index).digest() +
                      hashlib.sha256(manifest_bytes).digest() + b'\0' * 8)
            assert len(header) == HEADER_BYTES and len(index) == index_size
            file.seek(0)
            file.write(header + index)
        except BaseException:
            file.close()
            temporary.unlink(missing_ok=True)
            raise
    temporary.rename(output)
    result = dict(status='COMPLETE', pack=str(output.resolve()), bytes=total, sha256=vulkan.file_sha(output),
                  normalized_stages=len(stages), raw_stages=len(raw_rows), original_occurrences=raw['original_occurrences'],
                  all_spirv_resources_reflected=True, active_descriptors_and_vertex_inputs_only=True,
                  original_manifest_attribute_regex_corrected_offline=True, index_bytes=index_size, spirv_bytes=total_spirv,
                  source_identity=identity, tools=manifest['tools'],
                  hardware_rendering_verified=False, runtime_modules_or_pipelines_precreated=False)
    vulkan.write_json(output.with_suffix(output.suffix + '.json'), result)
    print(json.dumps(result, indent=2))


def self_check():
    prefix = '#version 320 es\n#define ENABLE_UNIFORM_BUFFER_OBJECTS 0\n#define ENABLE_MULTIDRAW_INDIRECT 0\n'
    frag = prefix + 'uniform vec4 PConstFloat[3];\nuniform bool PConstBool[2];\nuniform sampler2D PSampler1;\nout vec4 FragmentColor;\nvoid main() { FragmentColor=PConstFloat[1]; }\n'
    vert = prefix + 'uniform bool VConstBool[2];\nuniform vec4 VConstFloat[3];\nvoid main() { gl_Position=VConstFloat[0]; }\n'
    _, fs = vulkan.adapt(frag, False)
    _, vs = vulkan.adapt(vert, True)
    assert fs['fields'][1]['offset'] == 48 and vs['fields'][1]['offset'] == 32
    assert struct.unpack_from('<IIII', layout_bytes(fs, 0x8b30), 32) == (6, 1, 3, 0)
    assert struct.unpack_from('<IIII', layout_bytes(vs, 0x8b31), 32) == (4, 3, 2, 0)
    for bad in [dict(fs, parameter_binding=0), dict(fs, parameter_bytes=1),
                dict(fs, samplers=[dict(fs['samplers'][0], binding=2)]),
                dict(fs, fields=[dict(fs['fields'][0], offset=16)])]:
        try:
            layout_bytes(bad, 0x8b30)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid layout accepted')
    compiler = vulkan.NDK_TOOLS / 'glslc.exe'
    import subprocess
    with tempfile.TemporaryDirectory() as folder:
        for source, target in ((frag, 0x8b30), (vert, 0x8b31)):
            converted, layout = vulkan.adapt(source, target == 0x8b31)
            path, binary = Path(folder)/'stage.glsl', Path(folder)/'stage.spv'
            path.write_bytes(converted.encode('latin1'))
            subprocess.run([str(compiler), '--target-env=vulkan1.1', '-O', '-fshader-stage=' + ('vert' if target == 0x8b31 else 'frag'), str(path), '-o', str(binary)], check=True)
            data = binary.read_bytes()
            assert reflect(data, layout, target)
            for broken, wrong_layout, wrong_target in ((data[:-1], layout, target), (data, layout, 0x8b31 if target == 0x8b30 else 0x8b30),
                                                      (data, dict(layout, parameter_binding=3), target)):
                try:
                    reflect(broken, wrong_layout, wrong_target)
                except (ValueError, KeyError, IndexError):
                    pass
                else:
                    raise AssertionError('malformed or wrong-stage SPIR-V accepted')
    raw = b'//ShaderFileName:A\nvoid main(){}'
    variant = b'#version 150\n' + raw
    assert raw != variant and vulkan.sha(raw) != vulkan.sha(variant)
    assert original.normalize(raw) == original.normalize(variant)
    assert (0x8620, vulkan.sha(raw)) != (0x8804, vulkan.sha(raw))
    print('PASS: raw target/digest identities, normalization aliases, independent bool/std140 offsets, group indices, strict layouts and actual compiled SPIR-V reflection')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--audit-only', action='store_true')
    parser.add_argument('--patch', type=Path, default=ROOT/'original/patch.31.com.tt2kgames.Borderlands2.obb')
    parser.add_argument('--catalogue', type=Path, default=ROOT/'app/assets/bl2_shader_stages.bin')
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--raw-map', type=Path)
    parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        self_check()
    elif not args.output or not args.output.parent.is_dir():
        parser.error('provide a new output in an existing directory')
    elif args.audit_only:
        audit(args.patch, args.catalogue, args.output)
    elif not args.manifest or not args.raw_map:
        parser.error('packing requires --manifest and --raw-map')
    else:
        pack(args.manifest, args.raw_map, args.patch, args.catalogue, args.output)


if __name__ == '__main__':
    main()
