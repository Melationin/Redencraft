"""Compile official 26.1 light-occlusion faces into a runtime table."""

import argparse
import pathlib


def cpp_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace(
        '\x1f', '\\037') + '"'


def parse_face(field):
    if not field:
        return ()
    rectangles = []
    for rectangle in field.split(';'):
        values = tuple(int(value) for value in rectangle.split(':'))
        if len(values) != 4:
            raise ValueError(f'invalid face rectangle: {rectangle}')
        rectangles.append(values)
    return tuple(rectangles)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tsv', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()

    shapes = {tuple([()] * 6): 0}
    shape_rows = [tuple([()] * 6)]
    rect_spans = {(): (0, 0)}
    rect_list = []
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
        for face in faces:
            if face not in rect_spans:
                rect_spans[face] = (len(rect_list), len(face))
                rect_list.extend(face)
        shape_index = shapes.get(faces)
        if shape_index is None:
            shape_index = len(shape_rows)
            shapes[faces] = shape_index
            shape_rows.append(faces)
        states.append((key, shape_index))
    states.sort()

    text = [
        '// Generated from assets/light26_1/block_light_faces.tsv. Do not edit.',
        'static constexpr LightFaceRect kLightFaceRects[] = {'
    ]
    if not rect_list:
        text.append('    {},')
    else:
        for rect in rect_list:
            text.append('    {%d, %d, %d, %d},' % rect)
    text.append('};')
    text.append('static constexpr LightFaceShape kLightFaceShapes[] = {')
    for faces in shape_rows:
        parts = []
        present = 0
        for face in faces:
            if face:
                present |= 1 << len(parts)
            offset, count = rect_spans[face]
            parts.append('{%d, %d}' % (offset, count))
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
