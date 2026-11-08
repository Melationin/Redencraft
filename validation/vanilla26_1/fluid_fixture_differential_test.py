"""Compare the C++ W-02/M-03/H-01 probe with the recorded 26.1 golden.

This is a regression pin. It reruns only C++; the golden and raw server log are
produced separately by fluid_relative_validation.py against official 26.1.
"""

import argparse
import json
import pathlib
import subprocess


def read_probe(text):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if not line.startswith("{"):
            raise SystemExit("unexpected probe output: " + line[:200])
        rows.append(json.loads(line))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--golden", required=True)
    args = parser.parse_args()

    run = subprocess.run([args.probe], capture_output=True)
    if run.returncode != 0:
        raise SystemExit("probe failed: %r" % run.stderr[-800:])
    rows = read_probe(run.stdout.decode("utf-8", "replace"))
    golden = json.loads(pathlib.Path(args.golden).read_text(encoding="utf-8"))
    compared = 0
    for fixture, block in golden["fixtures"].items():
        cell_count = len(block["cells"])
        expected = block["states"]
        actual = {}
        for row in rows:
            if row.get("fixture") != fixture:
                continue
            key = (row.get("t"), row.get("cell"))
            if key in actual:
                raise SystemExit("%s duplicate observation %r" % (fixture, key))
            actual[key] = row.get("state")
        required = {(tag, index) for tag in expected
                    for index in range(cell_count)}
        if set(actual) != required:
            missing = required - set(actual)
            extra = set(actual) - required
            raise SystemExit("%s observation set differs: missing=%d extra=%d" %
                             (fixture, len(missing), len(extra)))
        diffs = []
        for tag in sorted(expected, key=lambda value: int(value[1:])):
            for index, wanted in enumerate(expected[tag]):
                got = actual[(tag, index)]
                if got != wanted:
                    diffs.append((tag, index, got, wanted))
                compared += 1
        if diffs:
            for tag, index, got, wanted in diffs[:20]:
                print("DIFF", fixture, tag, index,
                      "cpp=", got, "vanilla=", wanted)
            raise SystemExit("%s has %d mismatching cells" %
                             (fixture, len(diffs)))
    print("fluid fixture differential matched over %d cells" % compared)


if __name__ == "__main__":
    main()
