#!/usr/bin/env python3
"""Run a wide-area random-coordinate differential alignment check between
Mojang 26.1 server.jar and C++ ChunkGenerationPipeline."""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
import pathlib
import random
import shutil
import socket
import subprocess
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parent
REPO = ROOT.parents[1]
JAR_SHA1 = "3872a7f07a1a595e651aef8b058dfc2bb3772f46"


def find_jdk():
    env_java = os.environ.get("JAVA_25") or os.environ.get("JAVA_26")
    if env_java:
        java = pathlib.Path(env_java)
        return java, java.with_name("javac"), java.with_name("jar")
    bundled = REPO / "validation" / "random_tick_bench" / "runtime" / "jdk" / "bin"
    if (bundled / "java").exists():
        return bundled / "java", bundled / "javac", bundled / "jar"
    raise RuntimeError("Cannot find JDK 25+ binary")


def sha1(path: pathlib.Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def canonical_state(text: str) -> str:
    idx = text.find("[")
    if idx < 0:
        return text
    name = text[:idx]
    body = text[idx + 1 : -1]
    if not body:
        return name
    parts = sorted(p.strip() for p in body.split(",") if p.strip())
    return f"{name}[{','.join(parts)}]"


def expand_rle(palette, rle, canonicalize=False):
    if canonicalize:
        pal = [canonical_state(s) for s in palette]
    else:
        pal = palette
    out = []
    for idx, count in rle:
        out.extend([pal[idx]] * count)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--world-seed", type=int, default=849201745213)
    parser.add_argument("--rng-seed", type=int, default=20261117)
    parser.add_argument("--ow-random", type=int, default=256)
    parser.add_argument("--nether-random", type=int, default=40)
    parser.add_argument("--end-random", type=int, default=40)
    parser.add_argument("--include-anchors", action="store_true",
                        help="Also prepend fixed near-origin anchor chunks")
    parser.add_argument("--range-chunks", type=int, default=1875,
                        help="Max abs chunk coord (+-30000 blocks for 1875)")
    parser.add_argument("--cpp-probe", type=pathlib.Path,
                        default=REPO / "build" / "worldgen_differential_probe")
    parser.add_argument("--reuse-run", type=pathlib.Path, default=None,
                        help="Reuse existing vanilla output in run_dir and re-run C++ & compare")
    args = parser.parse_args()

    versions_jar = ROOT / "versions" / "26.1" / "server-26.1.jar"
    server_jar = ROOT / "server.jar"
    if server_jar.exists() and sha1(server_jar) != JAR_SHA1:
        raise SystemExit(f"unexpected server.jar SHA-1: {sha1(server_jar)}")
    if not versions_jar.exists():
        raise SystemExit(f"missing {versions_jar}")
    if not args.cpp_probe.exists():
        raise SystemExit(f"missing C++ probe binary: {args.cpp_probe}")

    if args.reuse_run is not None:
        run_dir = args.reuse_run
        spec_path = run_dir / "spec.txt"
        java_out = run_dir / "java_out.jsonl"
        cpp_out = run_dir / "cpp_out.jsonl"
        print(f"Reusing existing vanilla run directory: {run_dir}")
    else:
        java_bin, javac_bin, jar_bin = find_jdk()

        run_dir = (REPO / "build" / "worldgen-diff-runs" /
                   f"{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}-{uuid.uuid4().hex[:6]}")
        run_dir.mkdir(parents=True, exist_ok=True)
        print(f"Run directory: {run_dir}")

        # Build deterministic random world coordinates across all dimensions.
        prng = random.Random(args.rng_seed)
        seen = set()
        specs = []

        def add_chunk(dim, cx, cz, stages, min_sep=18):
            key = (dim, cx, cz)
            if key in seen:
                return False
            for edim, ecx, ecz in seen:
                if edim == dim and max(abs(cx - ecx), abs(cz - ecz)) <= min_sep:
                    return False
            seen.add(key)
            specs.append((args.world_seed, dim, cx, cz, stages))
            return True

        if args.include_anchors:
            add_chunk("minecraft:overworld", 0, 0, "NOISE,SURFACE,CARVERS,FEATURES,FULL", min_sep=-1)
            add_chunk("minecraft:overworld", 3, -2, "NOISE,SURFACE,CARVERS,FEATURES,FULL", min_sep=-1)

        ow_added = 0
        while ow_added < args.ow_random:
            cx = prng.randint(-args.range_chunks, args.range_chunks)
            cz = prng.randint(-args.range_chunks, args.range_chunks)
            # Keep outside world spawn area so pre-generated spawn chunks do not alter NOISE/SURFACE snapshots.
            if max(abs(cx), abs(cz)) <= 24:
                continue
            stages = "FULL" if ow_added == args.ow_random - 1 else "NOISE,SURFACE,CARVERS,FEATURES,FULL"
            if add_chunk("minecraft:overworld", cx, cz, stages):
                ow_added += 1

        ne_range = max(100, args.range_chunks // 2)
        ne_added = 0
        while ne_added < args.nether_random:
            cx = prng.randint(-ne_range, ne_range)
            cz = prng.randint(-ne_range, ne_range)
            stages = "FULL" if ne_added == args.nether_random - 1 else "NOISE,SURFACE,CARVERS,FEATURES,FULL"
            if add_chunk("minecraft:the_nether", cx, cz, stages):
                ne_added += 1

        if args.include_anchors:
            add_chunk("minecraft:the_end", 2, 0, "NOISE,SURFACE,CARVERS,FEATURES,FULL", min_sep=-1)
        end_added = 0
        while end_added < args.end_random:
            cx = prng.randint(-ne_range, ne_range)
            cz = prng.randint(-ne_range, ne_range)
            stages = "FULL" if end_added == args.end_random - 1 else "NOISE,SURFACE,CARVERS,FEATURES,FULL"
            if add_chunk("minecraft:the_end", cx, cz, stages):
                end_added += 1

        spec_path = run_dir / "spec.txt"
        with spec_path.open("w", encoding="utf-8") as f:
            for seed, dim, cx, cz, stages in specs:
                f.write(f"{seed} {dim} {cx} {cz} {stages}\n")

        # Compile WorldgenDiffAgent
        classes_dir = run_dir / "agent-classes"
        classes_dir.mkdir()
        libs = [str(p) for p in sorted((ROOT / "libraries").rglob("*.jar"))]
        compile_cp = os.pathsep.join([str(versions_jar), *libs])
        subprocess.run(
            [str(javac_bin), "-cp", compile_cp, "-d", str(classes_dir),
             str(ROOT / "WorldgenDiffAgent.java")],
            check=True,
        )
        manifest = run_dir / "MANIFEST.MF"
        manifest.write_text("Manifest-Version: 1.0\nPremain-Class: WorldgenDiffAgent\n\n",
                            encoding="ascii")
        agent_jar = run_dir / "worldgen-diff-agent.jar"
        subprocess.run(
            [str(jar_bin), "cfm", str(agent_jar), str(manifest), "-C", str(classes_dir), "."],
            check=True,
        )

        # Configure isolated vanilla server
        server_dir = run_dir / "vanilla-server"
        server_dir.mkdir()
        (server_dir / "eula.txt").write_text("eula=true\n", encoding="ascii")
        (server_dir / "server.properties").write_text(
            f"server-ip=127.0.0.1\n"
            f"server-port={free_port()}\n"
            f"online-mode=false\n"
            f"level-seed={args.world_seed}\n"
            f"level-name=world\n"
            f"spawn-protection=0\n"
            f"view-distance=4\n"
            f"simulation-distance=4\n"
            f"max-players=1\n"
            f"pause-when-empty-seconds=-1\n"
            f"max-tick-time=-1\n"
            f"allow-nether=true\n",
            encoding="ascii",
        )

        java_out = run_dir / "java_out.jsonl"
        cpp_out = run_dir / "cpp_out.jsonl"
        runtime_cp = os.pathsep.join([str(versions_jar), *libs, str(agent_jar)])

        print(f"Running Mojang 26.1 headless server ({len(specs)} chunks, world_seed={args.world_seed})...")
        t0 = time.monotonic()
        java_proc = subprocess.run(
            [
                str(java_bin),
                "-Xms512M",
                "-Xmx6G",
                f"-javaagent:{agent_jar}",
                f"-Dworldgen.diff.spec={spec_path}",
                f"-Dworldgen.diff.out={java_out}",
                f"-Dworldgen.diff.seed={args.world_seed}",
                "-cp",
                runtime_cp,
                "net.minecraft.server.Main",
                "--nogui",
            ],
            cwd=server_dir,
            capture_output=True,
            text=True,
            timeout=1800,
        )
        (run_dir / "java_server.log").write_text(java_proc.stdout + "\n" + java_proc.stderr, encoding="utf-8")
        if java_proc.returncode != 0 or "VANILLA_WORLDGEN_DIFF_OK" not in java_proc.stdout:
            print(java_proc.stdout[-2000:])
            print(java_proc.stderr[-2000:])
            raise SystemExit(f"Vanilla server run failed (code={java_proc.returncode})")
        print(f"Vanilla finished in {time.monotonic() - t0:.2f}s")

    print("Running C++ worldgen_differential_probe...")
    t1 = time.monotonic()
    cpp_proc = subprocess.run(
        [str(args.cpp_probe), str(REPO / "assets" / "worldgen26_1"),
         str(spec_path), str(cpp_out)],
        cwd=REPO,
        capture_output=True,
        text=True,
        timeout=1800,
    )
    if cpp_proc.returncode != 0 or "CPP_WORLDGEN_DIFF_OK" not in cpp_proc.stdout:
        print(cpp_proc.stdout)
        print(cpp_proc.stderr)
        raise SystemExit(f"C++ probe failed (code={cpp_proc.returncode})")
    print(f"C++ probe finished in {time.monotonic() - t1:.2f}s")

    def load_jsonl(path):
        data = {}
        with path.open("r", encoding="utf-8") as f:
            for line in f:
                obj = json.loads(line)
                k = (obj["dim"], obj["cx"], obj["cz"], obj["stage"])
                data[k] = obj
        return data

    jmap = load_jsonl(java_out)
    cmap = load_jsonl(cpp_out)

    agg = defaultdict(lambda: {
        "chunks": 0,
        "biome_total": 0,
        "biome_match": 0,
        "block_total": 0,
        "block_match": 0,
        "block_id_match": 0,
        "hm_total": 0,
        "hm_match": 0,
        "exact_chunks": 0,
        "diffs": Counter(),
    })

    chunk_reports = []
    for key in sorted(jmap.keys(), key=lambda k: (k[0], k[3], k[1], k[2])):
        jrec = jmap[key]
        crec = cmap[key]
        dim, cx, cz, stage = key

        j_biomes = expand_rle(jrec["biome_palette"], jrec["biome_rle"])
        c_biomes = expand_rle(crec["biome_palette"], crec["biome_rle"])
        # Align biome quart grid by world quart-Y if dimension height differs (e.g. End 256 vs noise height 128)
        jq_min = jrec["min_y"] >> 2
        cq_min = crec["min_y"] >> 2
        jq_cnt = jrec["height"] >> 2
        cq_cnt = crec["height"] >> 2
        q_start = max(jq_min, cq_min)
        q_end = min(jq_min + jq_cnt, cq_min + cq_cnt)
        b_match = 0
        b_total = jq_cnt * 16
        for qy in range(jq_min, jq_min + jq_cnt):
            j_off = (qy - jq_min) * 16
            if q_start <= qy < q_end:
                c_off = (qy - cq_min) * 16
                for i in range(16):
                    if j_biomes[j_off + i] == c_biomes[c_off + i]:
                        b_match += 1
            else:
                # Beyond C++ noise-height ProtoChunk range (e.g. End y>=128)
                pass

        j_blocks = expand_rle(jrec["block_palette"], jrec["block_rle"], canonicalize=True)
        c_blocks = expand_rle(crec["block_palette"], crec["block_rle"], canonicalize=True)
        jy0, jh = jrec["min_y"], jrec["height"]
        cy0, ch = crec["min_y"], crec["height"]
        y_lo = min(jy0, cy0)
        y_hi = max(jy0 + jh, cy0 + ch)

        blk_match = 0
        id_match = 0
        blk_total = (y_hi - y_lo) * 256
        local_diffs = Counter()
        for y in range(y_lo, y_hi):
            j_slice = (
                j_blocks[(y - jy0) * 256 : (y - jy0 + 1) * 256]
                if jy0 <= y < jy0 + jh
                else ["minecraft:air"] * 256
            )
            c_slice = (
                c_blocks[(y - cy0) * 256 : (y - cy0 + 1) * 256]
                if cy0 <= y < cy0 + ch
                else ["minecraft:air"] * 256
            )
            for jb, cb in zip(j_slice, c_slice):
                if jb == cb:
                    blk_match += 1
                    id_match += 1
                else:
                    jname = jb.split("[", 1)[0]
                    cname = cb.split("[", 1)[0]
                    if jname == cname:
                        id_match += 1
                    local_diffs[(jb, cb)] += 1

        # Compare heightmaps appropriate for stage
        if stage in ("FULL",):
            hm_keys = ("WORLD_SURFACE", "MOTION_BLOCKING")
        else:
            hm_keys = ("WORLD_SURFACE_WG", "OCEAN_FLOOR_WG", "MOTION_BLOCKING", "WORLD_SURFACE")
        hm_total = 0
        hm_match = 0
        for hk in hm_keys:
            jh = jrec["heightmaps"][hk]
            ch = crec["heightmaps"][hk]
            hm_total += len(jh)
            hm_match += sum(1 for a, b in zip(jh, ch) if a == b)

        bucket = agg[(dim, stage)]
        bucket["chunks"] += 1
        bucket["biome_total"] += b_total
        bucket["biome_match"] += b_match
        bucket["block_total"] += blk_total
        bucket["block_match"] += blk_match
        bucket["block_id_match"] += id_match
        bucket["hm_total"] += hm_total
        bucket["hm_match"] += hm_match
        if blk_match == blk_total and b_match == b_total:
            bucket["exact_chunks"] += 1
        bucket["diffs"].update(local_diffs)

        dom_biome = Counter(j_biomes).most_common(1)[0][0]
        chunk_reports.append({
            "dim": dim,
            "cx": cx,
            "cz": cz,
            "stage": stage,
            "dominant_biome": dom_biome,
            "biome_match_pct": 100.0 * b_match / b_total,
            "block_match_pct": 100.0 * blk_match / blk_total,
            "block_id_match_pct": 100.0 * id_match / blk_total,
            "block_diffs": blk_total - blk_match,
            "hm_match_pct": 100.0 * hm_match / hm_total if hm_total else 100.0,
            "top_diffs": [(f"{jb} -> {cb}", cnt) for (jb, cb), cnt in local_diffs.most_common(5)],
        })

    print("\n=== STAGE SUMMARY (Vanilla 26.1 vs C++) ===")
    print(f"{'Dimension':<22} {'Stage':<10} {'Chunks':>6} {'Exact':>6} {'Biome%':>9} {'Block%':>9} {'BlkID%':>9} {'HM%':>9}")
    summary_rows = []
    for (dim, stage) in sorted(agg.keys()):
        st = agg[(dim, stage)]
        b_pct = 100.0 * st["biome_match"] / st["biome_total"]
        blk_pct = 100.0 * st["block_match"] / st["block_total"]
        id_pct = 100.0 * st["block_id_match"] / st["block_total"]
        hm_pct = 100.0 * st["hm_match"] / st["hm_total"]
        print(f"{dim:<22} {stage:<10} {st['chunks']:>6} {st['exact_chunks']:>6} "
              f"{b_pct:>8.4f}% {blk_pct:>8.4f}% {id_pct:>8.4f}% {hm_pct:>8.4f}%")
        summary_rows.append({
            "dim": dim,
            "stage": stage,
            "chunks": st["chunks"],
            "exact_chunks": st["exact_chunks"],
            "biome_match_pct": b_pct,
            "block_match_pct": blk_pct,
            "block_id_match_pct": id_pct,
            "hm_match_pct": hm_pct,
            "top_diffs": [(f"{a} -> {b}", c) for (a, b), c in st["diffs"].most_common(12)],
        })

    print("\n=== FULL DISTRIBUTION & TOP MISMATCHES BY DIMENSION ===")
    for dim in sorted({r["dim"] for r in chunk_reports}):
        fulls = [r for r in chunk_reports if r["dim"] == dim and r["stage"] == "FULL"]
        if not fulls:
            continue
        blk_pcts = sorted(r["block_match_pct"] for r in fulls)
        id_pcts = sorted(r["block_id_match_pct"] for r in fulls)
        biomes = sorted({r["dominant_biome"] for r in fulls})
        exact_cnt = sum(1 for r in fulls if r["block_diffs"] == 0 and r["biome_match_pct"] == 100.0)
        ge_999 = sum(1 for p in blk_pcts if p >= 99.9)
        ge_99 = sum(1 for p in blk_pcts if p >= 99.0)
        median_blk = blk_pcts[len(blk_pcts) // 2]
        median_id = id_pcts[len(id_pcts) // 2]
        print(f"  {dim}: chunks={len(fulls)} distinct_dom_biomes={len(biomes)} "
              f"exact={exact_cnt} >=99.9%={ge_999} >=99.0%={ge_99} "
              f"min_blk={blk_pcts[0]:.4f}% median_blk={median_blk:.4f}% median_id={median_id:.4f}%")
        st = agg[(dim, "FULL")]
        print(f"    top FULL block diffs: {st['diffs'].most_common(8)}")

    print("\n=== WORST MISMATCHED CHUNKS (FULL, up to 15) ===")
    worst_full = sorted(
        [r for r in chunk_reports if r["stage"] == "FULL" and r["block_diffs"] > 0],
        key=lambda r: r["block_diffs"],
        reverse=True,
    )[:15]
    for r in worst_full:
        print(f"  {r['dim']} ({r['cx']:>5},{r['cz']:>5}) biome={r['biome_match_pct']:.2f}% "
              f"block={r['block_match_pct']:.4f}% blkID={r['block_id_match_pct']:.4f}% "
              f"diffs={r['block_diffs']:>5} dom={r['dominant_biome']} top={r['top_diffs'][:3]}")

    surf_carv_errs = [r for r in chunk_reports if r["stage"] in ("NOISE", "SURFACE", "CARVERS") and r["block_diffs"] > 0]
    if surf_carv_errs:
        print(f"\n=== NOISE/SURFACE/CARVERS MISMATCHES ({len(surf_carv_errs)} snapshots, showing up to 10) ===")
        for r in sorted(surf_carv_errs, key=lambda r: r["block_diffs"], reverse=True)[:10]:
            print(f"  {r['dim']} ({r['cx']:>5},{r['cz']:>5}) [{r['stage']:<7}] "
                  f"biome={r['biome_match_pct']:.2f}% block={r['block_match_pct']:.4f}% "
                  f"diffs={r['block_diffs']:>5} dom={r['dominant_biome']} top={r['top_diffs'][:3]}")

    report_path = run_dir / "diff_report.json"
    report_path.write_text(json.dumps({
        "world_seed": args.world_seed,
        "rng_seed": args.rng_seed,
        "summary": summary_rows,
        "chunks": chunk_reports,
    }, indent=2), encoding="utf-8")
    print(f"\nDetailed report saved to: {report_path}")


if __name__ == "__main__":
    main()
