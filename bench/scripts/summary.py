"""Median ns/key per (dist, n, algo) from a sweep CSV, plus NFSS speedup (other/nfss).

    python3 scripts/summary.py results/NAME.csv [--compare results/OTHER.csv]

--compare prints the change in NFSS median between two sweeps (for optimisation work).
"""
import csv
from pathlib import Path
import statistics
import sys
from collections import defaultdict


def load(path):
    cells = defaultdict(list)
    with open(path) as f:
        for row in csv.DictReader(f):
            time = row.get("median_ns", row.get("ns"))
            if time:
                cells[(row["dist"], row["type"], int(row["n"]), row["algo"])].append(float(time) / int(row["n"]))
    return {k: statistics.median(v) for k, v in cells.items()}


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    base = load(args[0])
    if "--compare" in args:
        other = load(args[args.index("--compare") + 1])
        print(f"{'dist':16} {'type':>4} {'n':>11} {'before':>9} {'after':>9} {'change':>8}")
        for (dist, typ, n, algo), before in sorted(base.items()):
            if algo == "nfss" and (dist, typ, n, algo) in other:
                after = other[(dist, typ, n, algo)]
                print(f"{dist:16} {typ:>4} {n:>11} {before:9.2f} {after:9.2f} {after / before - 1:+8.1%}")
        return
    cache = Path(args[args.index("--competitors") + 1]) if "--competitors" in args else Path(args[0]).with_name("competitors.csv")
    if cache.exists() and cache.resolve() != Path(args[0]).resolve():
        requested_cells = {k[:3] for k in base}
        for key, value in load(cache).items():
            if key[:3] in requested_cells and key[3] in ("bls", "pdq", "ips4o", "spread", "std"):
                base.setdefault(key, value)
    algos = sorted({a for _, _, _, a in base})
    candidates = [a for a in ("nfss", "radix", "classic") if a in algos]
    groups = defaultdict(list)
    print("```text")
    print(f"{'dist':16} {'type':>4} {'n':>11} " + " ".join(f"{a:>9}" for a in algos) + "   " + "   ".join(f"best/{a}" for a in candidates))
    for dist, typ, n in sorted({(d, t, n) for d, t, n, _ in base}):
        row = {a: base.get((dist, typ, n, a)) for a in algos}
        competitors = [v for a, v in row.items() if a not in ("nfss", "nfss_base", "radix", "classic") and v is not None]
        pattern = dist in ("sorted", "reverse") or dist.startswith(("organpipe", "nearlysorted", "runs", "tail"))
        for candidate in candidates:
            if not row.get(candidate):
                continue
            for label, reference in (("original", row.get("nfss_base")), ("best competitor", min(competitors, default=None))):
                if reference:
                    for subset in (("all",) if pattern else ("all", "without patterns")):
                        groups[candidate, label, subset].append(reference / row[candidate])
        ratios = [f"x{min(competitors) / row[a]:.2f}" if row.get(a) and competitors else "-" for a in candidates]
        print(f"{dist:16} {typ:>4} {n:>11} " + " ".join(f"{row[a]:9.2f}" if row[a] else f"{'-':>9}" for a in algos)
              + "   " + "   ".join(ratios))
    print("```")
    for (candidate, ref, subset), values in groups.items():
        print(f"- {candidate} vs {ref}, {subset} ({len(values)} cells): {statistics.geometric_mean(values):.3f}×")


if __name__ == "__main__":
    main()
