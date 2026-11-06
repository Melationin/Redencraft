"""Compare recorded vanilla 26.1 outputs with actual C++ probe outputs.

Native-only cases are explicitly unverified, never counted as passing parity.
There is no tolerance, sorting, or order normalization in the comparator.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
REQUIRED = {"fixture_ticks", "dedup_ticks", "overdue_ticks", "negative_orders",
            "fixture_events", "moving_entities", "moving_completion_order"}


def first_difference(expected, actual, path="$",):
    if isinstance(expected, dict) and isinstance(actual, dict):
        if expected.keys() != actual.keys():
            return {"path": path, "expected_keys": sorted(expected), "actual_keys": sorted(actual)}
        for key in expected:
            result = first_difference(expected[key], actual[key], f"{path}.{key}")
            if result:
                return result
        return None
    if isinstance(expected, list) and isinstance(actual, list):
        if len(expected) != len(actual):
            return {"path": path, "expected_length": len(expected), "actual_length": len(actual)}
        for index, (left, right) in enumerate(zip(expected, actual)):
            result = first_difference(left, right, f"{path}[{index}]")
            if result:
                return result
        return None
    if expected != actual:
        return {"path": path, "expected": expected, "actual": actual}
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    native = parser.add_mutually_exclusive_group(required=True)
    native.add_argument("--native", type=Path, help="compiled safesave_order_probe executable")
    native.add_argument("--native-json", type=Path, help="previously captured C++ result")
    parser.add_argument("--vanilla", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "comparison.json")
    parser.add_argument("--cpp-output", type=Path)
    args = parser.parse_args()
    if args.native:
        run = subprocess.run([str(args.native.resolve())], capture_output=True, text=True, check=True, timeout=60)
        actual = json.loads(run.stdout)
    else:
        actual = json.loads(args.native_json.read_text(encoding="utf-8-sig"))
    if args.cpp_output:
        args.cpp_output.write_text(json.dumps(actual, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    sources = args.vanilla or [ROOT / "vanilla/vanilla-order.json"]
    if not args.vanilla and (ROOT / "vanilla/vanilla-server-order.json").exists():
        sources.append(ROOT / "vanilla/vanilla-server-order.json")
    expected = {}
    for source in sources:
        entries = json.loads(source.read_text(encoding="utf-8-sig"))
        for case, value in entries.items():
            if case in expected and first_difference(expected[case], value):
                raise ValueError(f"Conflicting vanilla recordings for {case}: {source}")
            expected[case] = value
    results = {}
    for case, value in expected.items():
        if case not in actual:
            results[case] = {"match": False, "difference": "case absent from C++ result"}
        else:
            difference = first_difference(value, actual[case])
            results[case] = {"match": difference is None}
            if difference:
                results[case]["difference"] = difference
    missing_required = sorted(REQUIRED - expected.keys())
    failed = [case for case, result in results.items() if not result["match"]]
    native_only = sorted(actual.keys() - expected.keys())
    report = {
        "minecraft_version": "26.1",
        "source_test_suite": "D:/Java/SafeSave2/tests/server (fixtures adapted; SafeSave not loaded)",
        "vanilla_recordings": [str(path.resolve()) for path in sources],
        "passed_compared_cases": not failed and not missing_required,
        "compared": len(results), "matched": len(results)-len(failed),
        "cases": results, "missing_required_vanilla_cases": missing_required,
        "native_only_not_vanilla_verified": native_only,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(f"Compared {len(results)} cases: {len(results)-len(failed)} match, {len(failed)} differ.")
    for case in failed:
        print(f"DIFF {case}: {json.dumps(results[case]['difference'], ensure_ascii=False)}")
    if native_only:
        print("C++ only; NOT vanilla verified: " + ", ".join(native_only))
    if missing_required:
        print("Missing required vanilla recordings: " + ", ".join(missing_required))
    return 0 if report["passed_compared_cases"] else 1


if __name__ == "__main__":
    sys.exit(main())
