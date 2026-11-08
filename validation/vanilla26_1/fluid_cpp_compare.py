"""C++ side of the F-7 fluid differential.

Builds the SAME sealed channel as validation/vanilla26_1/fluid_validation.py,
places the source through setBlock (so onPlace queues the first tick), then
records the same channel cells after each tick. Output goes to
fluid-cpp-states.json in the same shape as the vanilla side.
"""
import json, pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent
exe = root.parent.parent / 'build-jemalloc' / 'Release' / 'redstone_cli.exe'

X0, X1, Y, Z = -2, 6, 80, 0
CELLS = [(x, Y, Z) for x in range(X0, X1 + 1)]

# The CLI does not expose step-by-step fluid inspection, so the harness is the
# C++ test binary below; this script only documents the fixture contract and
# compares the two JSON files once both sides have produced them.


def compare(cpp_path, vanilla_path):
    cpp = json.loads(pathlib.Path(cpp_path).read_text(encoding='utf-8'))
    vanilla = json.loads(pathlib.Path(vanilla_path).read_text(encoding='utf-8'))
    key = lambda r: (r['t'], r['cell'])
    a = {key(r): r['state'] for r in cpp}
    b = {key(r): r['state'] for r in vanilla}
    ticks = sorted({k[0] for k in a} | {k[0] for k in b})
    diffs = []
    for t in ticks:
        for i in range(len(CELLS)):
            av, bv = a.get((t, i)), b.get((t, i))
            if av != bv:
                diffs.append({'tick': t, 'cell': i, 'cpp': av, 'vanilla': bv})
    return diffs


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit('usage: fluid_cpp_compare.py <cpp.json> <vanilla.json>')
    d = compare(sys.argv[1], sys.argv[2])
    if d:
        print('FIRST DIFFERENCES:', json.dumps(d[:20], indent=1))
        raise SystemExit(1)
    print('identical across', len(CELLS), 'cells')