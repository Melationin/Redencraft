import ast, json, pathlib, sys
root = pathlib.Path(sys.argv[1]).resolve()
raw = (root / 'fluid-cpp-states.txt').read_bytes()
for enc in ('utf-16', 'utf-8-sig', 'utf-8'):
    try:
        text = raw.decode(enc)
        break
    except UnicodeDecodeError:
        continue
else:
    raise SystemExit('cannot decode')
recs = [ast.literal_eval(l.strip()) for l in text.splitlines() if l.strip()]
(root / 'fluid-cpp-states.json').write_text(json.dumps(recs, indent=1),
                                             encoding='utf-8')
print('cpp records', len(recs))