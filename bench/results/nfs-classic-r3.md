# NFS classic, round three

Across the full grid on an Apple M5 Pro, classic ran **2.101× as fast as Fran’s original** and **1.107× as fast as round-two classic**. Radix was **1.221× faster than the new classic**. All 680 correctness cells passed.

| Reference time ÷ new classic time | All 170 cells | 105 without ordering patterns |
|---|---:|---:|
| Fran’s original (`nfss_base`) | 2.101× | 1.829× |
| Round-two classic (`83a36d4`) | 1.107× | 1.170× |
| Radix (`6a99d65`) | 0.819× | 0.856× |

At 100M, Facebook IDs went from 9.37 ns/key in round two to 4.21 in new classic (2.23×). On uniform u64, new classic took 5.15 ns/key, round two 6.30, the original 8.48 and radix 4.22.

## Results by port

Each quick trial ran the same 34 distributions at 100K and 10M. The speedup divides the preceding header’s median time by the candidate’s median in each cell, then takes the geometric mean. A result above 1 favors the change. The cell examples give the preceding and candidate medians in ns/key. The 100M uniform pilot ran separately because the quick tier ends at 10M.

| Idea | Decision | Quick speedup | Largest effects and tradeoffs |
|---|---|---:|---|
| Sparse placement on existing small leaves | Dropped | 0.970×, then 0.992× | At 100K, 10,000-value inputs rose from 3.16 to 4.67 on the first trial. An order and occupancy probe plus local insertion reduced the overall loss, but 10M Facebook IDs still rose from 7.30 to 8.17. |
| Remove a partition level on large children | Kept, using sparse placement here | 1.014× | At 100M uniform, a larger histogram alone took 5.59 vs 5.56 for round two (1.004× on the quick grid). A bounded placement leaf in place of the next fragment pass took **4.70 vs 5.46**. At 10M, exponential fell from 5.10 to 4.16 and signed normal from 5.95 to 4.86. |
| Peel extreme tails | Kept with a tighter sample check | 1.022× | At 10M, heavy-tail fell from 10.61 to 5.53 and sentinel from 7.98 to 6.29. A looser first trial slowed 10M lognormal from 5.11 to 6.63. The retained gate brought that cell to 4.94 vs 4.93, while 100K lognormal rose from 5.01 to 6.07. |
| Two stable digit scatters on narrow integer leaves | Kept | 1.028× | At 10M, Facebook fell from 7.75 to 4.83 and SOSD dense from 4.79 to 3.96. At 100K, wiki fell from 4.15 to 3.35. |
| Four independent min/max lanes | Kept | 1.013× | At 100K, heavy-tail fell from 4.06 to 3.02, sentinel from 4.22 to 3.19, and lognormal from 6.34 to 5.26. The largest measured loss was 10M mirrored organ pipe, 2.67 to 2.88, which finishes in the unchanged run-merge path. |
| Size fragments, buckets and leaves from cache budgets | Kept | 1.003× | Rounding the histogram cutoff down to a power of two and the minimum fanout up brought 100K clustered from 9.18 to 5.15 against the preceding header. The earlier formula took 11.95 vs 9.88. At 100K, the 10% unsorted-tail case rose from 1.08 to 1.32. |

The first full sweep showed that a whole 10K array just exceeded the 8,192-key histogram cutoff. Starting another fragment pass there slowed 300-value inputs from 3.45 to 7.85 ns/key. The final dispatch lets a whole array use up to two histogram leaves’ worth of space, while recursive children keep the original cutoff. Remeasuring all 34 affected 10K cells moved their geomean against round two from 0.906× to 1.063×. On 300-value inputs the corrected median was 2.10 ns/key.

The [quick-trial CSV](nfs-classic-r3-quick.csv) preserves medians for each trial and the 10K medians before the dispatch correction. The [full-grid CSV](nfs-classic-r3.csv) preserves individual repetitions. In the latter, `nfss` is this round, `classic` is round two, and `radix` is the frozen radix branch.

## Cache budgets and x86 proxy

The header accepts three compile-time parameters, in bytes. Use the same definitions in translation units that share an instantiation.

| Parameter | Default | x86-like proxy |
|---|---:|---:|
| `NFS_CLASSIC_CACHE_LINE_BYTES` | 64 | 64 |
| `NFS_CLASSIC_L1D_BYTES` | 65,536 | 49,152 |
| `NFS_CLASSIC_L2_BYTES` | 2,097,152 | 1,048,576 |

A fragment spans eight cache lines. Bucket metadata must fit in L1D and its fragment staging can use up to half of L2. The target population per bucket is half the histogram-leaf capacity. The sparse slots have an L2 budget and the digit histograms share a bounded fraction of L2. These are configurable working-set estimates. With the defaults, u64 gets 64 keys per fragment, up to 2,048 buckets, an 8,192-key histogram limit and a 65,536-key placement limit. A uniform child at 100M therefore averages about 48,828 keys after the first scatter. With the proxy, the caps become 1,024 buckets and 32,768 placement keys.

| Quick grid on the M5 Pro | Vs original, all | Vs original, without patterns | Vs round two, all | Vs round two, without patterns |
|---|---:|---:|---:|---:|
| Default budgets | 2.156× | 1.860× | 1.084× | 1.140× |
| x86-like budgets | 2.138× | 1.899× | 1.082× | 1.165× |

The proxy lost to the original on two 10M cells, `nearlysorted` by 28.9% and `nearlysorted5` by 17.0%. Against round two, the proxy exceeded the 3% threshold on 15 cells. At 100K these were normal (+7.9%), fewunique (+8.4%), exponential (+9.1%), sosd_normal (+4.3%), tail10 (+11.6%), signednormal (+3.0%). At 10M these were organpipe (+9.7%), nearlysorted (+50.6%), clustered (+5.4%), sosd_lognormal (+4.8%), dup300 (+7.2%), organpipe_overlap (+3.4%), runs2 (+4.9%), runs16 (+20.0%), nearlysorted5 (+29.3%).

Both binaries used GCC 16.2 and native Arm instructions on the same M5 Pro. The proxy tests sensitivity to cache budgets. Throughput on an x86 CPU remains unmeasured.

## Auxiliary space

| Piece | Storage bound |
|---|---|
| Sparse placement and the shallower large-child path | Integers map distance from the interval low to equal-width, power-of-two slots. Floats scale the numerical interval linearly. One shared slot array holds four times the next power of two of the eligible leaf size plus a 64-slot guard. The occupancy bitmap uses one bit per slot and one guard word. Probing at most two words leaves the input intact on fallback. On success, insertion repairs collisions as occupied slots drain. |
| Tail peeling | A fixed 64-key sample, two in-place partitions when both tails qualify, and exact bounds for each of the three pieces. The pieces reuse one workspace and are processed sequentially. |
| Digit scatters | One temporary array of at most a leaf of keys, reused from the fragment/placement workspace. Two unsigned offset tables, each capped by the cache budget and by a fixed multiple of leaf length. Only the offsets from the interval’s low endpoint contribute digits. |
| Four-lane bounds | Four minima, four maxima and scalar loop state. |
| Cache-derived scheduling | For a fixed configuration and key type, fanout, fragment storage, the run-merge buffer and all leaf capacities have fixed maxima. The u64 default workspace arrays and one swap fragment total at most 2,477,784 bytes, excluding allocator overhead and recursive boundary vectors. |

Each distribution frame retains at most one array of endpoints bounded by the larger of the fanout and the histogram-bin cap. A child retaining more than 95% of its parent can stall only a bounded number of times before comparison fallback. Other children shrink by at least 5%. This bounds the depth by O(log n). The run merger likewise uses bounded scratch and logarithmic recursion. With the existing in-place comparison fallback, total auxiliary space stays **O(log n)**. Equal-width distribution, analytical child bounds, fragment permutation and the final insertion pass remain in the general sort.

## Cells over the 3% threshold

New classic beat the original in all 170 cells. These 16 cells took over 3% longer than round two. A dash means the measured loss did not exceed 3%.

| Input | New ns/key | Round two ns/key | Original ns/key | Extra vs round two | Extra vs original |
|---|---:|---:|---:|---:|---:|
| sosd_normal 10K | 5.000 | 4.100 | 6.000 | +22.0% | — |
| tail10 10K | 1.000 | 0.850 | 3.950 | +17.6% | — |
| tail1 10K | 0.700 | 0.600 | 3.850 | +16.7% | — |
| dup1000 10M | 3.639 | 3.227 | 4.133 | +12.8% | — |
| exponential 10K | 4.600 | 4.200 | 6.300 | +9.5% | — |
| runs16 10K | 2.800 | 2.600 | 3.550 | +7.7% | — |
| sorted 1M | 0.118 | 0.110 | 0.255 | +7.3% | — |
| organpipe 10M | 0.288 | 0.270 | 7.321 | +6.7% | — |
| dup300 100K | 5.670 | 5.355 | 6.360 | +5.9% | — |
| signednormal 100K | 4.755 | 4.570 | 6.935 | +4.0% | — |
| sorted 100M | 0.122 | 0.117 | 0.245 | +4.0% | — |
| nearlysorted 100M | 9.006 | 8.668 | 9.021 | +3.9% | — |
| dup1000 1M | 4.806 | 4.628 | 5.766 | +3.8% | — |
| runs1000keys 10K | 2.900 | 2.800 | 3.500 | +3.6% | — |
| nearlysorted 10K | 3.200 | 3.100 | 3.300 | +3.2% | — |
| tail1 10M | 0.585 | 0.567 | 7.336 | +3.1% | — |

The largest relative loss was sosd_normal 10K. Against the faster of the two references it cost 0.900 ns/key. The retained changes saved 5.16 ns/key on 100M Facebook IDs and 6.20 ns/key on 10M heavy-tail keys.

## Verification and reproduction

The 170-cell result combines the 34 remeasured 10K cells from `2256562` with 136 larger cells from the full sweep at `e82d241`. The final change affects only top-level arrays between one and two histogram leaves. For u64 and f64 under the default budgets, that is 8,193 through 16,384 keys. All four implementations were retimed together in the 10K slice.

The default full run rotated new classic, round two, the original and radix in one process per distribution. Each sort started with a fresh copy of the same seed-42 input. Generation, copying and validation were outside the timer. The harness used ten repetitions at 10K, 100K and 1M, five at 10M and three at 100M. All 5,168 timed outputs passed the sorted-order and multiset checks. The 105-cell subset excludes sorted, reverse, organ pipes, both swap rates, sorted runs and sorted-prefix tails. Every cell has equal weight.

The parallel check passed 34 distributions × four key types × five sizes (1, 2, 17, 1,000 and 10,000,000). The four implementations each sorted all 680 cases. The final header also passed the 680 cases again and 952 additional boundary cases. The single Clang ASan/UBSan run, before the 10K dispatch correction, checked sorted order and exact before/after bit multisets on 19 native arithmetic types × 13 sizes × five input families, including signed zeros, infinities, NaNs and integer extremes. The final dispatch passed a separate uninstrumented check of exact bit multisets and sorted order on the same 19 types and five families at six boundary sizes per type.

```sh
CXX=g++-16 cmake -S bench -B bench/build -G Ninja -DNFS_CLASSIC_REV=83a36d4 -DNFS_RADIX_REV=6a99d65
cmake --build bench/build
TIER=quick ALGOS=nfss,classic,nfss_base,radix bench/scripts/sweep.sh local-r3 all 8
ALGOS=nfss,classic,nfss_base,radix bench/scripts/sweep.sh local-r3 all 8
CHECK_SIZES=1,2,17,1000,10000000 ALGOS=nfss,classic,nfss_base,radix bench/scripts/sweep.sh check all
python3 bench/scripts/summary.py bench/results/local-r3.csv
```

For the proxy build, add `-DCMAKE_CXX_FLAGS="-DNFS_CLASSIC_CACHE_LINE_BYTES=64 -DNFS_CLASSIC_L1D_BYTES=49152 -DNFS_CLASSIC_L2_BYTES=1048576"` when configuring a second build directory. Set `NFSBENCH` to that binary and use a fresh sweep name.

The summary joins the [existing external competitor cache](competitors.csv). All timings collected in this round were for NFS implementations.
