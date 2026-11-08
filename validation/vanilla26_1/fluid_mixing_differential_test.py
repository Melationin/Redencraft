"""Compare the C++ M-01 mixing probe with the recorded 26.1 golden.

The committed golden is the compact transition + stable-endpoint form produced
from validation/vanilla26_1/fluid_mixing_validation.py; the raw server log and
full per-tick JSON stay in build-fluid-validation/ (ignored). This test re-runs
only the C++ half, so it is a regression pin, not a fresh vanilla run.
"""
import argparse
import ast
import json
import pathlib
import subprocess

CASES = ('m01-source', 'm01-flowing')
TICKS = 10
TAGS = ('T0', 'T1', 'T2', 'T10')


def read_probe(text):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if line:
            rows.append(ast.literal_eval(line))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--probe', required=True)
    ap.add_argument('--golden', required=True)
    args = ap.parse_args()
    golden = json.loads(pathlib.Path(args.golden).read_text(encoding='utf-8'))
    diffs = []
    compared = 0
    for case in CASES:
        out = subprocess.run([args.probe, case, str(TICKS)],
                             capture_output=True)
        if out.returncode != 0:
            raise SystemExit(f'probe {case} failed: {out.stderr[-400:]!r}')
        rows = read_probe(out.stdout.decode('utf-8', 'replace'))
        by_tag = {}
        for row in rows:
            if row['case'] != case or row['t'] not in TAGS:
                continue
            by_tag.setdefault(row['t'], {})[row['cell']] = row['state']
        for tag in TAGS:
            expected = golden[case][tag]
            actual = [by_tag.get(tag, {}).get(i) for i in range(7)]
            compared += 7
            if actual != expected:
                diffs.append((case, tag, actual, expected))
    if diffs:
        for case, tag, actual, expected in diffs[:10]:
            print('DIFF', case, tag, 'cpp=', actual, 'vanilla=', expected)
        raise SystemExit(f'{len(diffs)} mismatching tick/cell groups')
    print('mixing differential matched over %d cells' % compared)


if __name__ == '__main__':
    main()
