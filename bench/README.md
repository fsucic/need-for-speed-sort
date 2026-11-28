# NFS Sort benchmark

A rebuild of the benchmark in [arXiv:2607.11850](https://arxiv.org/abs/2607.11850) §5. The paper says its benchmark is public, but the repo only ships `nfss.h`, so this harness follows the protocol the paper describes:

- Competitors: std::sort, PDQSort, IPS4o (sequential), Boost Spreadsort and Balanced Learned Sort.
- Every algorithm gets the same input in each run. Only the sort call is timed (`steady_clock`), and every output is checked: it must be sorted and contain the same multiset of keys.
- Synthetic inputs: uniform, sorted, reverse, organ pipe, normal, nearly sorted, few unique, exponential, clustered, plus SOSD's lognormal, normal, dense-uniform and sparse-uniform.
- Real inputs: SOSD books, fb, osm and wiki, shuffled with the first n keys taken.
- The original paper protocol uses ten runs at sizes from 10^4 to 10^9. The iteration and final grids below use fewer repetitions at the largest sizes.

## Setup (macOS, Apple Silicon)

```sh
brew install boost tbb cmake ninja gcc zstd
scripts/setup.sh                 # competitor sorts at pinned commits -> third_party/
scripts/download_sosd.sh         # ~16 GB of SOSD keys -> data/
CXX=g++-16 cmake -S . -B build -G Ninja && ninja -C build
```

The build needs GCC because BLS uses GCC variable-length arrays. With clang, BLS fails to compile.

## Run

```sh
build/nfsbench --dist uniform --n 1e6 --runs 10            # one cell, summary on stderr
scripts/sweep.sh baseline all 8                            # grid up to 10^8 -> results/baseline.csv (resumable)
python3 scripts/summary.py results/baseline.csv            # median ns/key and NFSS speedup
python3 scripts/summary.py results/baseline.csv --compare results/try1.csv   # NFSS before/after
```

`--type` picks `u64` (default), `i64`, `u32` or `f64`. SOSD files contain u64 keys. The default sweep keeps those keys intact. An explicit `--type` converts the same sampled keys for cross-type validation. `u32` keeps the low 32 bits, `i64` interprets the full 64 bits as signed, and `f64` rounds to double precision.

## Iteration and the competitor cache

Each cell generates one seed-42 input. Every repetition starts from a fresh copy and rotates algorithm order. One process handles multiple sizes for a distribution and reads its SOSD file once. Its sample for any size is reproducible when that size runs alone.

From `bench/`:

```sh
scripts/sweep.sh competitors all 8                   # fill missing external measurements
TIER=quick scripts/sweep.sh candidate all 8          # 100K and 10M, candidates only
scripts/sweep.sh candidate all 8                     # fill the other final sizes
python3 scripts/summary.py results/candidate.csv     # join results/competitors.csv
scripts/sweep.sh check all                           # untimed checks at 1, 2, 17 and 1,000
CHECK_SIZES=1,2,17,1000,10000000 scripts/sweep.sh check all
```

Use a fresh candidate result name when the implementation changes. A quick run and its final run can share a name for an unchanged binary. The default timing schedule is ten repetitions through 1M, five at 10M and three at 100M. `ALGOS` selects candidates and defaults to `nfss,nfss_base`. Timing sweeps take the same file lock. Validation runs up to twelve distributions concurrently and supports all four types. For a single binary with both branches, configure CMake with `-DNFS_RADIX_REV=<commit> -DNFS_CLASSIC_REV=<commit>` and use `ALGOS=radix,classic,nfss_base`.

The cache stores per-cell median times in nanoseconds and repetition counts for BLS, PDQSort, IPS4o, Spreadsort and std::sort. The sweep fills only missing measurements. The summary accepts `--competitors PATH` for a cache in another directory. Existing external timings in an older self-contained CSV take precedence when summarizing that file.

## Differences from the paper

- The paper ran on one core of an AMD EPYC 9354. macOS can't pin a thread to a core, so the harness asks for the highest QoS class, which keeps it on the performance cores. Close other heavy work before taking numbers.
- The paper doesn't give parameters for the synthetic distributions; the values here are in `generate()` in `bench.cpp`. The SOSD distributional sets are drawn at random instead of SOSD's quantile construction, which gives the same distribution.
- fb and wiki only have 200M keys, so the harness can't sample 10^9 from them without replacement and stops with an error. The paper doesn't say how it handled this.
- `fb` is used raw. BLS's scripts strip its outliers first.
