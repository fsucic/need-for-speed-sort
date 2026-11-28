# Cross-porting classic and low-memory radix

On the Apple M5 Pro, the two sorts finished **within 0.02%** across 170 paired cells (R5 / Radix 2 throughput 0.99985×). Classic won 73, tied 16 and lost 81. Against their respective predecessors, R5 reached **1.038× R4** and Radix 2 reached **1.137× the previous radix**. Each peaked at **896 KiB** of heap scratch.

## What actually differs

Fran’s classic sort divides the observed value range proportionally. Radix reads a group of bits. With keys from 0 to 999 and four buckets, classic puts 253 in bucket 1. Radix puts it in bucket 0 because its first boundary is 256. Both sequences sort correctly. What changes is how many keys land together and how much work remains inside each bucket.

Holding the rest of R5 fixed, interpolation gained **0.2%** across the quick grid. On signed-normal doubles it produced a largest bucket of **0.2%** at 10M, versus **28.0%** for a single encoded-bit map. The shipped radix handles the two signs separately. Bit selection was **4.2% faster** on the two wide integer spans in the matched test.

The finishing method mattered more on Facebook. At 100M, a value split with R5’s comparison finish took **4.50 ns/key**. Giving that same value split a radix counting finish took **3.36 ns/key**. This hybrid was an experiment. R5 ships with contiguous value intervals at every split. [Controlled timings](cross-port-ablation.csv.gz)

## Full paired grid

Each cell contributes its median ns/key once to its geometric mean. Lower times are faster. The last column divides the original’s all-cell mean by the time in that row. [All 7,752 timed sorts](cross-port.csv)

| Sort | All 170, ns/key | 105 without ordered patterns | 80 smooth | vs original, all |
|---|---:|---:|---:|---:|
| Radix 2 | 2.077 | 2.903 | 3.495 | 2.728× |
| Classic R5 | 2.077 | 2.905 | 3.564 | 2.728× |
| Previous low-memory radix | 2.360 | 3.384 | 4.002 | 2.400× |
| Classic R4 | 2.156 | 3.073 | 3.775 | 2.628× |
| Classic R3 | 2.718 | 3.943 | 4.219 | 2.085× |
| Fran’s original | 5.665 | 7.148 | 7.487 | 1.000× |

| Throughput comparison | All 170 | Without ordered patterns | Smooth |
|---|---:|---:|---:|
| Classic R5 / Radix 2 | 1.000× | 0.999× | 0.981× |
| Radix 2 / Previous low-memory radix | 1.137× | 1.166× | 1.145× |
| Classic R5 / Classic R4 | 1.038× | 1.058× | 1.059× |
| Classic R5 / Classic R3 | 1.309× | 1.358× | 1.184× |
| Radix 2 / Classic R3 | 1.309× | 1.358× | 1.207× |
| Classic R5 / Fran’s original | 2.728× | 2.461× | 2.101× |
| Radix 2 / Fran’s original | 2.728× | 2.462× | 2.142× |

The smooth 16 are uniform, normal, signed normal, exponential, heavy tail, both spans, the four SOSD generators, Facebook, books, wiki, OSM and clustered. The 105-cell subset adds the four duplicate families and sentinel. The 13 ordered distributions cover sorted, reverse, swaps, organ pipes, runs and sorted-prefix tails.

Six single-threaded sorts received fresh copies of the same seed-42 input in one process per distribution, in rotating order. The five sizes used 10, 10, 10, 5 and 3 repetitions. Signed normal used f64 and the other 33 distributions used u64. Every timed output passed the order and multiset-fingerprint checks. All cell medians were positive. Ties use the recorded times before rounding.

### Head to head by distribution

Radix 2 time divided by R5 time. Above 1 favors classic. The last two columns cover all five sizes.

| Distribution | 10K | 100K | 1M | 10M | 100M | Geomean | R5 W/T/L |
|---|---:|---:|---:|---:|---:|---:|---:|
| uniform | 1.00 | 1.02 | 0.92 | 0.97 | 1.04 | 0.988× | 2/1/2 |
| normal | 1.00 | 1.00 | 0.98 | 0.98 | 1.05 | 0.999× | 1/1/3 |
| signednormal | 1.12 | 0.98 | 0.94 | 0.97 | 0.96 | 0.991× | 1/0/4 |
| exponential | 0.83 | 0.93 | 0.98 | 1.01 | 0.99 | 0.944× | 1/0/4 |
| heavytail | 0.83 | 0.76 | 0.99 | 0.99 | 0.99 | 0.907× | 0/0/5 |
| span1e19 | 1.00 | 0.99 | 0.93 | 1.00 | 1.00 | 0.984× | 2/1/2 |
| span1p3e12 | 0.95 | 1.01 | 0.94 | 1.34 | 1.17 | 1.071× | 3/0/2 |
| sosd_lognormal | 0.90 | 0.89 | 1.01 | 1.03 | 1.03 | 0.972× | 3/0/2 |
| sosd_normal | 1.07 | 0.98 | 0.97 | 0.96 | 1.01 | 1.000× | 2/0/3 |
| sosd_dense | 1.00 | 1.00 | 1.01 | 1.04 | 1.09 | 1.027× | 3/1/1 |
| sosd_sparse | 0.97 | 1.02 | 0.95 | 0.98 | 1.04 | 0.991× | 2/0/3 |
| fb | 1.00 | 1.00 | 0.98 | 0.80 | 1.02 | 0.958× | 1/1/3 |
| books | 0.98 | 1.01 | 0.97 | 0.93 | 0.97 | 0.973× | 1/0/4 |
| wiki | 1.00 | 0.88 | 0.77 | 0.79 | 0.99 | 0.881× | 0/1/4 |
| osm | 0.89 | 0.99 | 1.02 | 1.00 | 1.04 | 0.987× | 3/0/2 |
| clustered | 1.36 | 0.97 | 0.95 | 0.96 | 0.97 | 1.032× | 1/0/4 |
| fewunique | 1.00 | 0.99 | 1.02 | 1.00 | 1.00 | 1.000× | 1/1/3 |
| dup300 | 1.07 | 1.02 | 1.02 | 1.01 | 1.00 | 1.022× | 4/0/1 |
| dup1000 | 0.98 | 1.07 | 1.02 | 1.00 | 1.00 | 1.015× | 3/0/2 |
| dup10000 | 0.93 | 1.03 | 1.01 | 1.00 | 1.02 | 0.997× | 4/0/1 |
| sentinel | 2.00 | 1.39 | 0.51 | 1.67 | 1.63 | 1.307× | 4/0/1 |
| sorted | 1.00 | 1.06 | 1.02 | 1.01 | 1.00 | 1.017× | 4/1/0 |
| reverse | 1.00 | 1.00 | 1.01 | 1.04 | 0.99 | 1.006× | 2/2/1 |
| nearlysorted | 1.14 | 1.06 | 1.01 | 1.00 | 1.01 | 1.043× | 4/0/1 |
| nearlysorted5 | 0.97 | 0.96 | 1.01 | 1.00 | 1.01 | 0.992× | 3/0/2 |
| organpipe | 1.00 | 0.96 | 1.01 | 1.00 | 1.06 | 1.004× | 3/1/1 |
| organpipe_overlap | 0.98 | 1.00 | 1.00 | 1.00 | 1.01 | 0.996× | 2/1/2 |
| organpipe_mirror | 0.95 | 1.01 | 1.01 | 0.99 | 1.00 | 0.992× | 3/0/2 |
| runs2 | 1.00 | 0.99 | 1.01 | 1.00 | 1.02 | 1.002× | 2/1/2 |
| runs16 | 0.96 | 1.00 | 0.98 | 0.99 | 1.00 | 0.985× | 2/0/3 |
| runs1000keys | 0.92 | 0.94 | 0.96 | 0.99 | 1.05 | 0.971× | 1/0/4 |
| tail1 | 1.00 | 0.98 | 1.02 | 1.02 | 1.00 | 1.003× | 2/1/2 |
| tail10 | 1.00 | 0.99 | 1.01 | 0.99 | 1.03 | 1.002× | 2/1/2 |
| tail40 | 1.00 | 1.01 | 0.98 | 0.99 | 1.00 | 0.995× | 1/1/3 |

## Where R4 spent the time

A separate instrumented build counted key visits and bucket occupancy on the same inputs. These are the nonempty/allocated buckets and largest child of the largest scatter after any tail peel. Depth starts at zero. The terminal share divides the keys finished on that path by the input length. The timing builds contained no counters. [Profiles for all five sizes](cross-port-profile.csv)

| Input | Sort | Occupancy | Largest child | Max depth | Terminal path and share |
|---|---:|---:|---:|---:|---:|
| fb 10M | Classic R4 | 2,048/2,048 | 6,280 | 1 | sparse insertion, 100.0% |
| fb 10M | Previous low-memory radix | 1,152/1,152 | 10,495 | 1 | two-digit counts, 100.0% |
| fb 10M | Classic R5 | 2,048/2,048 | 6,280 | 1 | offset pages, 100.0% |
| fb 10M | Radix 2 | 1,152/1,152 | 10,495 | 1 | two-digit counts, 100.0% |
| fb 100M | Classic R4 | 2,048/2,048 | 61,797 | 4 | sparse insertion, 90.6% |
| fb 100M | Previous low-memory radix | 576/1,054 | 194,584 | 3 | two-digit counts, 96.6% |
| fb 100M | Classic R5 | 1,919/2,048 | 64,490 | 2 | offset pages, 100.0% |
| fb 100M | Radix 2 | 576/1,054 | 194,584 | 3 | two-digit counts, 96.6% |
| wiki 1M | Classic R4 | 479/489 | 6,467 | 1 | sparse insertion, 99.9% |
| wiki 1M | Previous low-memory radix | 224/224 | 14,080 | 1 | two-digit counts, 99.4% |
| wiki 1M | Classic R5 | 479/489 | 6,467 | 1 | sparse insertion, 90.2% |
| wiki 1M | Radix 2 | 441/448 | 7,065 | 1 | two-digit counts, 98.8% |
| wiki 10M | Classic R4 | 2,030/2,048 | 17,325 | 2 | sparse insertion, 100.0% |
| wiki 10M | Previous low-memory radix | 1,787/1,801 | 20,355 | 1 | two-digit counts, 99.9% |
| wiki 10M | Classic R5 | 2,030/2,048 | 17,325 | 2 | sparse insertion, 99.1% |
| wiki 10M | Radix 2 | 1,787/1,801 | 20,355 | 1 | two-digit counts, 99.9% |
| heavytail 10M | Classic R4 | 2,048/2,048 | 5,073 | 2 | dense counts, 99.0% |
| heavytail 10M | Previous low-memory radix | 1,025/1,923 | 10,053 | 2 | dense counts, 99.0% |
| heavytail 10M | Classic R5 | 257/270 | 39,126 | 2 | dense counts, 99.0% |
| heavytail 10M | Radix 2 | 257/481 | 39,296 | 2 | dense counts, 99.0% |

**Facebook.** Both old sorts reached depth 1 at 10M. R4 used sparse insertion on the whole sample and shifted 1.48 previously written keys per input key. Radix finished 100.0% with its two histograms. At 100M, failed R4 sparse attempts covered 76,288,292 key visits. Its crowded-leaf repartitions covered 73,453,908. A key can be visited twice. R4’s largest first child had 61,797 keys, above its 32,768-key leaf cutoff. R5’s largest had 64,490 and fit its 65,535-key offset leaf. It scattered the central range once and finished the offsets in numerical pages. In the matched R5 run, widening the padding around the sampled center raised 100M Facebook time from 4.50 to 5.53 ns/key. All Facebook, heavy-tail and wiki timings used unsigned integers.

**Heavy tail.** About 99% of the keys lie in 0…2²⁰. At 10M, R4’s extrema scans visited 2.00 input lengths. Previous radix visited 1.01 by carrying the bounds of the sampled center. R5 carries those bounds and chooses value intervals that fit its count table. It can count a dense range even when it contains more than 65,535 keys. Disabling that dense-range policy raised R5’s 100M heavy-tail time from 2.15 to 3.53 ns/key in the matched run. Radix 2 can do the same. At 100M, the four times in order R4 / R5 / previous radix / Radix 2 were 3.91 / 2.07 / 3.91 / 2.06 ns/key.

**Wiki.** At 1M, previous radix finished 99.4% through two-digit counting. R4 finished 99.9% through sparse insertion. At 10M, R5’s half-word scatter handled the whole input and could feed staged keys straight into a child leaf. In the full grid R5 / Radix 2 took 3.02 / 2.33 ns/key at 1M and 2.99 / 2.37 at 10M. The radix counting leaf retained the advantage at both sizes.

## Ablations

Each quick row is a paired before/after throughput comparison on all 34 distributions at 100K and 10M. Each table row names the change from its own starting point. Small differences can move with code layout and timing noise. [Per-cell medians](cross-port-ablation.csv.gz) · [Variant source diffs](cross-port-variants.tar.gz)

### R4 into radix

| Change | All 68 | Smooth 32 |
|---|---:|---:|
| Use only bit-selected buckets for floating-point partitions | 0.996× | 0.976× |
| Hash raw keys, separate keys and counts, adapt the sample and active table width | 1.027× | 1.006× |
| Merge comparable runs eight keys at a time | 1.006× | 0.999× |
| Copy ordered stretches and merge displaced keys eight at a time | 1.012× | 1.003× |
| Carry the largest key during sparse output | 1.039× | 1.065× |
| Repartition crowded leaves using smaller bit ranges | 0.996× | 1.005× |
| Select radix fanout against the L1 leaf target | 1.005× | 1.004× |
| Use sampled magnitude-bit windows for mixed-sign floats | 1.008× | 1.020× |
| Stage narrow keys in the unused halves of the input words | 1.009× | 1.010× |
| Read staged keys directly into eligible counting leaves | 1.008× | 1.013× |

Packed full-block cycles and inherited integer bounds already existed in low-memory radix. Both old tables already allowed 32,768 slots. R4 changed the active width, layout, sampling rate and identity comparison. On Fran’s 50-value doubles, a separate rotation measured 1.086 ms for previous radix, 0.960 ms with only raw-bit hashing/equality, and 0.850 ms with the full frequency-table port. The older radix interpolated numeric endpoints for some floating-point partitions. Radix 2 selects magnitude bits within each sign and mirrors the negative buckets.

The packed-range scatter changed Radix 2 from 3.55 to 3.25 ns/key at 10M Facebook IDs. Crowded-leaf subdivision cost 0.4% across the quick grid and gained 0.5% on its smooth subset.

### Radix into classic

| Change | All 68 | Smooth 32 |
|---|---:|---:|
| Carry the bounds of the sampled center into recursion | 1.003× | 1.007× |
| Count long dense ranges and size the fanout for the count table | 1.010× | 1.016× |
| Use one eighth of the sampled center width as padding | 0.997× | 0.998× |
| Enable dense-range counting and its fanout rule in the final driver | 0.996× | 0.996× |
| Enable half-word and packed-range scatters | 1.007× | 1.014× |
| Add packed-range scatter to half-word scatter | 0.997× | 1.003× |
| Use numerical pages with 16-bit offsets and comparison networks | 0.999× | 1.003× |

Both headers can scatter narrow integers into the unused halves of the input words. On wider eligible intervals, each word retains enough low bits to recover its original source key while the remaining bits hold a destination residue. The destination bucket’s lower bound recovers the full key. The decision depends on numerical range and key width.

At 100M Facebook IDs, disabling offset pages in the final R5 driver took 11.71 ns/key. Enabling them took 4.50. At 10M, a separate direct-bitmap page trial took 9.08 ns/key and a page with a four-key target and scalar sorting took 9.58. Buffering eight staged values per destination raised the 10M Facebook time from 4.64 to 5.79 ns/key in the packed-scatter trial.

### Holding the driver fixed

These comparisons keep the R5 prechecks, recursion, fanout policy, workspace and guards. They are separate paired runs from the six-sort full grid. “Bits” changes its main map and inverse bounds. The same finishing code can take a different path when given different bounds. The other experiments substitute a narrow integer leaf. The arithmetic version chooses an interval width near the square root of the span. It groups first by offset within the interval and then by interval number. Its first pass wraps across intervals, making this experimental leaf a mixed-radix sort.

| Main map / narrow leaf | Quick 68 vs R5 | Smooth 32 vs R5 | FB 10M ns/key | FB 100M | Wiki 1M | Heavy tail 100M |
|---|---:|---:|---:|---:|---:|---:|
| Value / ordered offset pages (shipped R5) | 1.000× | 1.000× | 4.04 | 4.50 | 3.02 | 2.15 |
| Bits / same ordered offset pages | 0.998× | 0.987× | 3.97 | 5.49 | 2.98 | 2.11 |
| Value / two counted bit digits | 1.014× | 1.024× | 3.46 | 3.36 | 2.56 | 2.13 |
| Bits / two counted bit digits | 1.005× | 0.995× | 3.33 | 4.48 | 2.56 | 2.13 |
| Value / two arithmetic coordinates | 1.008× | 1.012× | 3.73 | 3.59 | 2.84 | 2.14 |

## Fran’s 1M-double workload

Median milliseconds over 15 rotations. Every algorithm received the same doubles. All 810 outputs matched the full reference sort. [Replica](../fran_f64.cpp) · [Medians](cross-port-fran-f64.csv)

| Distribution | Original | R3 | R4 | R5 | Previous radix | Radix 2 |
|---|---:|---:|---:|---:|---:|---:|
| Uniform | 6.304 | 4.783 | 4.458 | 4.472 | 4.421 | 4.149 |
| Normal | 7.302 | 4.680 | 4.458 | 4.415 | 4.254 | 4.115 |
| Exponential | 6.719 | 4.626 | 4.389 | 4.386 | 4.281 | 4.859 |
| 50 distinct values | 2.204 | 2.300 | 0.853 | 0.849 | 1.189 | 0.866 |
| Four clusters | 9.453 | 7.552 | 5.981 | 6.023 | 5.756 | 6.089 |
| Sorted | 0.236 | 0.216 | 0.216 | 0.216 | 0.217 | 0.216 |
| Reverse | 0.385 | 0.366 | 0.370 | 0.369 | 0.367 | 0.367 |
| 1% random swaps | 5.992 | 5.521 | 1.020 | 1.027 | 1.472 | 1.190 |
| Organ pipe | 7.247 | 3.575 | 2.894 | 2.915 | 3.749 | 2.965 |

Across the nine cases, R5 ran at 1.016× Radix 2 throughput. R5 led on exponential doubles, while Radix 2 led on uniform and normal doubles. This organ pipe interleaves ascending random values between opposite ends.

## Memory, worst case and validation

The allocation probe counted heap requests between sort entry and return, excluding input storage, allocator metadata and stack. It checked the result against a reference and verified that each call released all scratch and had at most one live allocation. [Per-call measurements](cross-port-memory.csv)

| Path | Classic R5 | Radix 2 |
|---|---:|---:|
| Empty, singleton or already sorted | 0 B | 0 B |
| Distribution, u32 | 762,032 B | 516,360 B |
| Distribution, u64 / i64 / f64 | 786,952 B | 852,488 B |
| Frequency table, u64 | 589,824 B | 589,824 B |
| Overlapping runs, all four types | 917,504 B | 917,504 B |

R5’s fixed u32 distribution workspace grew from R4’s 516,360 B to 762,032 B to hold the short-offset pages and their cursors. The overlapping-run merge still sets the overall peak.

R5 reserves five rows of bucket boundaries. Radix 2 reserves one row per eight key bits plus one. Running out of rows on a distribution step invokes comparison sorting. Failed sparse placement abandons scratch with the input intact. In a successful placement, a key can pass entries from at most two bitmap words. Hash lookup gives up after 32 probes. R5’s offset pages use less than twice the leaf length before fixed alignment padding. Its largest sorting network handles 256 keys. Larger individual intervals use an O(k log k) comparison sort.

An accepted ordered split shrinks its larger side to at most seven eighths. A bounded-buffer run merge bisects the larger run and rotates if neither trimmed side fits. The overall bounds are O(n log n) worst-case time and O(log n) stack plus one fixed heap buffer. GCC reported largest sampling frames of 9,248 B for both sorts and distribution frames up to 2,400 B for R5 and 2,336 B for Radix 2. These measurements are per frame.

Each header passed 624 standard check-tier cases and 624 more at 9,999, 65,535, 65,536 and 300,000 keys. Combined ASan/UBSan passed 797 cases per header plus a separate 440-case exact-reference probe. ASan leak detection was disabled. The allocation probe checked releases independently. The 440 cases used an independent seed, uniform and irregular gaps, signed ranges crossing zero, unsigned ranges near the maximum, leaf boundaries, subnormals, both zeros, infinities and NaN payloads. They passed with Apple’s system g++ (Clang) and GNU g++ 16.2.

All timings used an Apple M5 Pro, native Arm code, GCC 16.2, O3 and NDEBUG. Both new headers default to a 64 B cache line, 32 KiB L1D and 1 MiB L2. Use the NFS_R5_ or NFS_LOWMEM2_ prefix with CACHE_LINE_BYTES, L1D_BYTES and L2_BYTES to override them. The 128 B / 128 KiB / 16 MiB override passed the 440-case probe with 128-bit arithmetic disabled. Both headers compiled to x86-64 objects. Native x86 timings remain unmeasured.

## Reproduce

From the bench directory, with the existing datasets and dependency headers installed. The sweep saves completed cells. Choose a fresh result name for an independent replay. [Classic R5 header](../../nfss_r5.h) · [Radix 2 header](../../nfss_radix_lowmem2.h)

~~~sh
CXX=g++-16 cmake -S . -B build/cross-port -G Ninja
cmake --build build/cross-port
export NFSBENCH=build/cross-port/nfsbench
ALGOS=classic5,lowmem2 scripts/sweep.sh check all
CHECK_SIZES=9999,65535,65536,300000 ALGOS=classic5,lowmem2 scripts/sweep.sh check all
ALGOS=lowmem2,classic5,nfss,nfss_base scripts/sweep.sh cross-port-replay all
build/cross-port/fran_f64
~~~

The sanitizer build used RelWithDebInfo with these CMake flags.

~~~sh
-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
~~~
