"""F-7 fluid differential regression.

Runs the C++ probe for the same fixture the vanilla harness used, then compares
its per-tick cell states against the recorded 26.1 golden file.

This is a REGRESSION pin over a recorded golden, not a fresh vanilla run. The
golden was produced by validation/vanilla26_1/fluid_validation.py against the
official 26.1 dedicated server (SHA-1 3872a7f07a1a595e651aef8b058dfc2bb3772f46).
"""
import argparse, ast, json, pathlib, subprocess, sys

TICKS = 12


def read_probe(text):
    records = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("{"):
            records.append(ast.literal_eval(line))
    return records


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--probe', required=True)
    ap.add_argument('--golden', required=True)
    args = ap.parse_args()

    out = subprocess.run([args.probe, str(TICKS)], capture_output=True)
    if out.returncode != 0:
        raise SystemExit('probe failed: %r' % out.stderr[-400:])
    text = out.stdout.decode('utf-8', 'replace')
    cpp = read_probe(text)
    vanilla = json.loads(pathlib.Path(args.golden).read_text(encoding='utf-8'))

    key = lambda r: (r['t'], r['cell'])
    # Reject duplicate keys before collapsing into dicts: a silent overwrite
    # would hide a broken sampler or probe that emitted a cell twice.
    for name, rows in (('cpp', cpp), ('vanilla', vanilla)):
        keys = [key(r) for r in rows]
        if len(keys) != len(set(keys)):
            raise SystemExit('%s has duplicate observations' % name)
    a = {key(r): r['state'] for r in cpp}
    b = {key(r): r['state'] for r in vanilla}
    missing = set(b) - set(a)
    extra = set(a) - set(b)
    if missing or extra:
        raise SystemExit('observation sets differ: missing=%d extra=%d'
                         % (len(missing), len(extra)))
    diffs = [k for k in sorted(a) if a[k] != b[k]]
    if diffs:
        for k in diffs[:20]:
            print('DIFF', k, 'cpp=', a[k], 'vanilla=', b[k])
        raise SystemExit('%d mismatching observations' % len(diffs))
    print('fluid differential matched over %d observations' % len(a))


if __name__ == '__main__':
    main()