"""Validate the checked-in 26.1 optical asset and generated C++ table."""

import argparse
import hashlib
import json
import pathlib
import tempfile

import generate_cpp
import generate_light_occlusion


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tsv', required=True)
    parser.add_argument('--names', required=True)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--faces', required=True)
    parser.add_argument('--faces-source', required=True)
    args = parser.parse_args()

    tsv = pathlib.Path(args.tsv)
    names = pathlib.Path(args.names)
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding='utf-8'))
    faces = pathlib.Path(args.faces)
    if manifest['jar_sha1'] != '3872a7f07a1a595e651aef8b058dfc2bb3772f46':
        raise SystemExit('unexpected official jar SHA-1')
    if sha256(tsv) != manifest['tsv_sha256']:
        raise SystemExit('optical TSV hash differs from manifest')
    if sha256(names) != manifest['names_sha256']:
        raise SystemExit('block-name list hash differs from manifest')
    if sha256(faces) != manifest['faces_sha256']:
        raise SystemExit('face TSV hash differs from manifest')

    known_blocks = set(names.read_text(encoding='utf-8').splitlines())
    rows = [line for line in tsv.read_text(encoding='utf-8').splitlines() if line]
    if len(rows) != manifest['rows']:
        raise SystemExit('optical row count differs from manifest')
    keys = set()
    shaped_keys = set()
    for number, line in enumerate(rows, 1):
        fields = line.split('\t')
        if len(fields) != 7:
            raise SystemExit(f'row {number}: expected seven TSV fields')
        key = (fields[0], fields[1])
        if key in keys:
            raise SystemExit(f'row {number}: duplicate state {key}')
        keys.add(key)
        if fields[0] not in known_blocks:
            raise SystemExit(f'row {number}: block is absent from block_names.txt')
        for field in fields[2:4]:
            value = int(field)
            if not 0 <= value <= 15:
                raise SystemExit(f'row {number}: invalid light value {value}')
        for field in fields[4:]:
            if field not in ('0', '1'):
                raise SystemExit(f'row {number}: invalid boolean {field}')
        if fields[5] == '1':
            shaped_keys.add(key)

    face_rows = [line for line in
                 faces.read_text(encoding='utf-8').splitlines() if line]
    if len(face_rows) != manifest['face_rows']:
        raise SystemExit('face row count differs from manifest')
    face_keys = set()
    for number, line in enumerate(face_rows, 1):
        fields = line.split('\t')
        if len(fields) != 8:
            raise SystemExit(f'face row {number}: expected eight TSV fields')
        key = (fields[0], fields[1])
        if key in face_keys:
            raise SystemExit(f'face row {number}: duplicate state {key}')
        face_keys.add(key)
        if key not in shaped_keys:
            raise SystemExit(f'face row {number}: state does not use light shape')
        for field in fields[2:]:
            if not field:
                continue
            if len(field.split(';')) != 1:
                raise SystemExit(f'face row {number}: expected one rectangle')
            values = [int(value) for value in field.split(':')]
            if len(values) != 4:
                raise SystemExit(f'face row {number}: invalid rectangle')
            if not (0 <= values[0] < values[1] <= 1_000_000 and
                    0 <= values[2] < values[3] <= 1_000_000):
                raise SystemExit(f'face row {number}: rectangle out of range')
    if face_keys != shaped_keys:
        raise SystemExit('face states do not equal useShapeForLightOcclusion states')

    with tempfile.TemporaryDirectory() as temporary:
        generated = pathlib.Path(temporary) / 'light_metadata.inc'
        old_argv = __import__('sys').argv
        try:
            __import__('sys').argv = [
                'generate_cpp.py', '--tsv', str(tsv), '--out', str(generated)]
            generate_cpp.main()
        finally:
            __import__('sys').argv = old_argv
        if generated.read_bytes() != pathlib.Path(args.source).read_bytes():
            raise SystemExit('src/light_metadata.inc is stale')
        generated_faces = pathlib.Path(temporary) / 'light_occlusion.inc'
        try:
            __import__('sys').argv = [
                'generate_light_occlusion.py', '--tsv', str(faces),
                '--out', str(generated_faces)]
            generate_light_occlusion.main()
        finally:
            __import__('sys').argv = old_argv
        if generated_faces.read_bytes() != pathlib.Path(
                args.faces_source).read_bytes():
            raise SystemExit('src/light_occlusion.inc is stale')
    print(f'optical metadata valid: {len(rows)} states, {len(face_rows)} face states')


if __name__ == '__main__':
    main()
