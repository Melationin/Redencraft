"""Compile official 26.1 light-occlusion faces into a runtime table."""

import argparse
import pathlib


def cpp_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace(
        '\x1f', '\\037') + '"'


def parse_face(field):
    if not field:
        return None
    rectangles = []
    for rectangle in field.split(';'):
        values = tuple(int(value) for value in rectangle.split(':'))
        if len(values) != 4:
            raise ValueError(f'invalid face rectangle: {rectangle}')
        rectangles.append(values)
    if len(rectangles) != 1:
        raise ValueError('expected one rectangle per exported face')
    return rectangles[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tsv', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()

    shapes = {tuple([None] * 6): 0}
    shape_rows = [tuple([None] * 6)]
    states = []
    for number, line in enumerate(
            pathlib.Path(args.tsv).read_text(encoding='utf-8').splitlines(), 1):
        if not line:
            continue
        fields = line.split('\t')
        if len(fields) != 8:
            raise SystemExit(f'{args.tsv}:{number}: expected 8 fields')
        key = fields[0] + '\x1f' + fields[1]
        faces = tuple(parse_face(field) for field in fields[2:])
        shape_index = shapes.get(faces)
        if shape_index is None:
            shape_index = len(shape_rows)
            shapes[faces] = shape_index
            shape_rows.append(faces)
        states.append((key, shape_index))
    states.sort()

    text = [
        '// Generated from assets/light26_1/block_light_faces.tsv. Do not edit.',
        'static constexpr LightFaceShape kLightFaceShapes[] = {'
    ]
    for faces in shape_rows:
        parts = []
        present = 0
        for face in faces:
            if face is not None:
                present |= 1 << len(parts)
            if face is None:
                parts.append('{}')
            else:
                parts.append('{%d, %d, %d, %d}' % face)
        text.append('    {%s, %d},' % (', '.join(parts), present))
    text.append('};')
    text.append('static constexpr OfficialLightFaceState kOfficialLightFaceStates[] = {')
    for key, shape_index in states:
        text.append('    {%s, %d},' % (cpp_string(key), shape_index))
    text.append('};')
    text.append('static_assert(std::size(kOfficialLightFaceStates) == %d);' %
                len(states))
    pathlib.Path(args.out).write_text('\n'.join(text) + '\n', encoding='utf-8')
    print(f'generated {len(states)} face states and {len(shape_rows)} unique masks')


if __name__ == '__main__':
    main()
