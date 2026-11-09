"""Compare the C++ block-light probe with the official 26.1 fixture."""

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
        if line.startswith("BLOCKLIGHT_JSON "):
            payload = json.loads(line[len("BLOCKLIGHT_JSON "):])
            candidate[str(payload["stage"])] = payload["block_light"]
    golden = json.loads(pathlib.Path(args.golden).read_text(encoding="utf-8"))
    if candidate != golden["stages"]:
        raise SystemExit("C++ block-light stages differ from official golden")
    print("block-light differential matched: %d stages, %d cells" %
          (len(candidate), len(candidate["1"])))


if __name__ == "__main__":
    main()
