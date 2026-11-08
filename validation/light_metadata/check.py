"""Validate the checked-in 26.1 optical asset and generated C++ table."""

import argparse
import hashlib
import json
import pathlib
import tempfile

import generate_cpp


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tsv', required=True)
    parser.add_argument('--names', required=True)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--source', required=True)
    args = parser.parse_args()

    tsv = pathlib.Path(args.tsv)
    names = pathlib.Path(args.names)
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding='utf-8'))
    if manifest['jar_sha1'] != '3872a7f07a1a595e651aef8b058dfc2bb3772f46':
        raise SystemExit('unexpected official jar SHA-1')
    if sha256(tsv) != manifest['tsv_sha256']:
        raise SystemExit('optical TSV hash differs from manifest')
    if sha256(names) != manifest['names_sha256']:
        raise SystemExit('block-name list hash differs from manifest')

    known_blocks = set(names.read_text(encoding='utf-8').splitlines())
    rows = [line for line in tsv.read_text(encoding='utf-8').splitlines() if line]
    if len(rows) != manifest['rows']:
        raise SystemExit('optical row count differs from manifest')
    keys = set()
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
    print(f'optical metadata valid: {len(rows)} unique states')


if __name__ == '__main__':
    main()
