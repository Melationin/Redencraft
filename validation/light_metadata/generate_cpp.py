"""Compile the official 26.1 block-light TSV into the runtime lookup table."""

import argparse
import pathlib


def cpp_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace(
        '\x1f', '\\037') + '"'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tsv', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    rows = []
    for number, line in enumerate(
            pathlib.Path(args.tsv).read_text(encoding='utf-8').splitlines(), 1):
        if not line:
            continue
        fields = line.split('\t')
        if len(fields) != 7:
            raise SystemExit(f'{args.tsv}:{number}: expected 7 fields')
        block, props, emission, dampening, can_occlude, use_shape, sky = fields
        key = block + '\x1f' + props
        rows.append((key, int(emission), int(dampening),
                     int(can_occlude), int(use_shape), int(sky)))
    rows.sort()
    text = [
        '// Generated from assets/light26_1/block_light.tsv. Do not edit.',
        'static constexpr OfficialLightState kOfficialLightStates[] = {'
    ]
    for key, emission, dampening, can_occlude, use_shape, sky in rows:
        text.append(
            '    {%s, %d, %d, %s, %s, %s},' %
            (cpp_string(key), emission, dampening,
             'true' if can_occlude else 'false',
             'true' if use_shape else 'false',
             'true' if sky else 'false'))
    text.append('};')
    text.append('static_assert(std::size(kOfficialLightStates) == %d);' % len(rows))
    pathlib.Path(args.out).write_text('\n'.join(text) + '\n', encoding='utf-8')
    print(f'generated {len(rows)} official light states')


if __name__ == '__main__':
    main()
