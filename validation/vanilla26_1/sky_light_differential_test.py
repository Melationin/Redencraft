"""Compare the C++ sky-light probe with the official 26.1 fixture."""

import argparse
import json
import pathlib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--golden", required=True)
    args = parser.parse_args()
    import subprocess
    output = subprocess.check_output([args.probe], text=True, encoding="utf-8")
    candidate = {}
    for line in output.splitlines():
        if line.startswith("SKYLIGHT_JSON "):
            payload = json.loads(line[len("SKYLIGHT_JSON "):])
            candidate[str(payload["stage"])] = {
                "sky_light": payload["sky_light"],
                "raw_missing": payload["raw_missing"],
            }
    golden = json.loads(pathlib.Path(args.golden).read_text(encoding="utf-8"))
    if candidate != golden["stages"]:
        raise SystemExit("C++ sky-light stages differ from official golden")
    cells = len(candidate["1"]["sky_light"])
    if any(len(stage["sky_light"]) != cells for stage in candidate.values()):
        raise SystemExit("C++ sky-light stages have inconsistent cell counts")
    raw_flags = sum(len(stage["raw_missing"])
                    for stage in candidate.values())
    print("sky-light differential matched: %d stages, %d cells each, "
          "%d sky observations + %d raw-layer flags" %
          (len(candidate), cells, len(candidate) * cells, raw_flags))


if __name__ == "__main__":
    main()
