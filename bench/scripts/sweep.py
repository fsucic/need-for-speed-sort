"""Batch sizes per distribution, reuse complete measurements and serialize timing sweeps."""
import csv
import fcntl
import itertools
import os
import subprocess
import sys
import statistics
from collections import defaultdict
from pathlib import Path

FIELDS = ["dist", "type", "n", "algo", "run", "ns"]
CACHE_FIELDS = ["dist", "type", "n", "algo", "runs", "median_ns", "status", "lower_bound_ns"]
EXTERNAL = ["bls", "pdq", "ips4o", "ips2ra", "spread", "std"]


def key(row):
    return row["dist"], row["type"], int(row["n"]), row["algo"], int(row["run"])


def save(path, fields, rows):
    temporary = path.with_suffix(".tmp")
    with temporary.open("w") as target:
        writer = csv.DictWriter(target, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows.values())
    os.replace(temporary, path)


def collect(path, algorithms, binary, distributions, type_override, schedule, cache=False):
    rows = {}
    fields = CACHE_FIELDS if cache else FIELDS
    if path.exists():
        with path.open() as source:
            reader = csv.DictReader(source)
            if reader.fieldnames != fields:
                raise ValueError(f"unexpected columns in {path}")
            for row in reader:
                k = (row["dist"], row["type"], int(row["n"]), row["algo"]) if cache else key(row)
                if k in rows:
                    raise ValueError(f"duplicate measurement in {path}: {k}")
                rows[k] = row
    for dist in distributions:
        typ = type_override or ("f64" if dist == "signednormal" else "u64")
        batches = defaultdict(list)
        for n, reps in schedule:
            if cache:
                missing = tuple(a for a in algorithms if (dist, typ, n, a) not in rows)
            else:
                missing = tuple(a for a in algorithms if any((dist, typ, n, a, r) not in rows for r in range(reps)))
            if missing:
                batches[missing].append((n, reps))
        for missing, sizes in batches.items():
            cmd = [binary, "--dist", dist, "--type", typ, "--algos", ",".join(missing),
                   "--n", ",".join(str(n) for n, _ in sizes), "--runs", ",".join(str(r) for _, r in sizes)]
            with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True) as process:
                reader = csv.DictReader(process.stdout, fieldnames=FIELDS)
                for n, reps in sizes:
                    expected = {(dist, typ, n, a, r) for a in missing for r in range(reps)}
                    measured = list(itertools.islice(reader, len(expected)))
                    if len(measured) != len(expected) or {key(row) for row in measured} != expected:
                        raise ValueError(f"incomplete batch for {dist}, {typ}, {n}, {missing}")
                    if cache:
                        for algo in missing:
                            times = [int(row["ns"]) for row in measured if row["algo"] == algo]
                            rows[dist, typ, n, algo] = dict(zip(CACHE_FIELDS, (
                                dist, typ, n, algo, reps, statistics.median(times), "measured", "")))
                    else:
                        replaced = {k[:4] for k in expected}
                        rows = {k: value for k, value in rows.items() if k[:4] not in replaced}
                        rows.update((key(row), row) for row in measured)
                    save(path, fields, rows)
                if next(reader, None) is not None or process.wait():
                    raise RuntimeError(f"benchmark failed for {dist}, {typ}, {missing}")
    print(f"{path}: {len(rows)} records", flush=True)


def main():
    name, binary, algos, tier, max_exp, repeats, typ, *distributions = sys.argv[1:]
    if tier not in ("quick", "full"):
        sys.exit("TIER must be quick or full")
    exponents = (5, 7) if tier == "quick" else range(4, int(max_exp) + 1)
    schedule = [(10**e, (10 if e <= 6 else 5 if e == 7 else 3) if repeats == "tiered" else int(repeats))
                for e in exponents if e <= int(max_exp)]
    requested = list(dict.fromkeys(algos.split(",")))
    competitors = [a for a in requested if a in EXTERNAL]
    if name == "competitors" and not competitors:
        competitors = EXTERNAL
    candidates = [] if name == "competitors" else [a for a in requested if a not in EXTERNAL]
    with Path("build/timing.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if competitors:
            collect(Path("results/competitors.csv"), competitors, binary, distributions, typ, schedule, cache=True)
        if candidates:
            collect(Path("results") / f"{name}.csv", candidates, binary, distributions, typ, schedule)


if __name__ == "__main__":
    main()
