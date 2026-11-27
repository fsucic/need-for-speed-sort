# Classic implementation notes

The implementation is in `f03d856`. It starts from Fran’s original header. It keeps equal-width value intervals, analytical child bounds, the 90-key fragment scatter and permutation, a whole-array insertion pass and a comparison fallback. The [full sweep](nfs-classic-final.md) includes each cell’s median ratios.

## Implementation

- Integer classification uses a fixed-point multiply-high on compilers with a 128-bit product. Inverting that classifier gives exact child endpoints without converting keys to double. The portable path divides by a precomputed integral interval width. Whole-array indices and counts use size_t. Leaf offsets are unsigned and bounded by the 10,000-key leaf size.
- The fragment pass adds a whole fragment to the bucket count when it flushes, then adds the final tail. The original fragment placement and spillover algorithm remains in place.
- The leaf prefix loop accumulates every crowded bucket size into an independent reduction. Crossing 401 no longer prevents the total from reaching the recursion threshold of 500. In a leaf that can finish locally, the scatter pre-sorts collisions of three or more keys, then merges adjacent pairs while copying back. Already ordered-looking output can go directly to the final insertion pass. A singleton integer interval is emitted from counts.
- A child retaining over 95% of its parent gets measured bounds before the next attempt. Two consecutive stalled refinements still lead to std::sort on the next stall. Small inputs use insertion sort before allocating a workspace. Sampling jitters the position within each stratum so a repeated run length cannot force every sample onto a run boundary.
- The ordered-input front end accepts up to four long runs or a sorted prefix or suffix covering at least half the input. It trims the merge overlap. When both sides exceed the original fragment workspace size, it splits and rotates until one side fits. The temporary merge buffer stays within 180,180 keys. The general sort still performs its final insertion cleanup.

## What the trials changed

A fixed sampling stride landed on the first key of each 1,000-key run at 10M. In the three-trial pilot that version took 7.69 ns/key. Jittered strata took 5.11 in the five-trial follow-up, against 7.31 for the original. On 10K sparse swaps, always using the fused leaf did extra work. Sampling scattered output recovered the simpler copy and final insertion path when local order survived. A separate probe covered 1% swaps on a normal distribution and independently shuffled 64-key blocks.

A corrected reciprocal division in the scatter inner loop gave exact integer quotients but needed a multiply and correction after the high product. That version took 7.62 ns/key on 10M runs of 1,000 against 7.22 for its paired original. The final classifier directly defines the interval map by its multiply-high result, then inverts that same map at bucket boundaries. Whole-fragment counting was introduced in the same trial, so their individual gains are unknown.

## Paper edits

Sections 3.2.2–3.2.7 would need to describe natural-run merges and their bounded workspace, stratified samples, exact integer interval arithmetic, fresh bounds on stalled recursion, singleton intervals, and the fused leaf before the final insertion pass. Numeric fast paths also place NaNs last. The high-level scatter, analytical recursion, comparison fallback and final-pass description still applies. The merge uses a constant-sized element buffer and logarithmic recursion. The original asymptotic space target survives.

The remaining engineering questions are the cost of bounded rotations on very large interleaved runs and the leaf-density crossover on different cache sizes. The GCC build with the 128-bit feature macro disabled matched std::sort on signed full-width values at 9,999, 10,001 and 1M keys. That exercises the portable arithmetic on this Mac. x86 throughput remains unmeasured.

## Validation

The correctness grid covered 34 distributions × four types × five sizes, for 680 cells. Sizes were 1, 2, 17, 1,000 and 10,000,000. Every cell checked the new implementation and the frozen original for sorted order and the harness’s multiset fingerprint. The three non-native SOSD types convert the sampled u64 values. The timed leaderboard uses native u64 for SOSD and f64 for signed normal.

The final parallel harness run repeated all 680 cells across the four implementations in 9.5 seconds wall time.

The separate Clang AddressSanitizer and UndefinedBehaviorSanitizer check compared exact before/after bit multisets across 19 arithmetic types, 13 sizes and five input families. It included empty subranges, signed extrema, adjacent large integers, subnormals, signed zeros, infinities and NaNs. A GCC run sorted 2,147,483,665 uint16 keys and verified every output against expected counts.

## Additional shapes

These five-trial medians are in ns/key. Each trial ran all four headers in rotated order and compared every output with a copy independently sorted by std::sort. The suite exercises a descending suffix, mixed directions, misleading regular samples, duplicates with rare new values and a second distribution of sparse swaps.

| Input | Keys | Radix | Classic | Original | Round one |
|---|---:|---:|---:|---:|---:|
| deadzone | 10,000 | 7.50 | 10.10 | 547.40 | 7.40 |
| nearlynormal | 1,000,000 | 0.98 | 6.82 | 7.06 | 1.07 |
| sortedblocks | 1,000,000 | 4.97 | 4.65 | 6.61 | 5.02 |
| zipf | 1,000,000 | 1.18 | 4.46 | 4.31 | 2.91 |
| suffix | 1,000,000 | 1.51 | 1.50 | 7.81 | 6.54 |
| three_reverse | 1,000,000 | 3.60 | 3.85 | 7.27 | 6.28 |
| four_mixed | 1,000,000 | 4.22 | 4.61 | 6.93 | 7.77 |
| salted_duplicates | 1,000,000 | 4.32 | 11.84 | 29.76 | 9.47 |
| single_sample_trap | 1,000,000 | 4.21 | 3.70 | 5.89 | 3.76 |
| uspan:1e12 | 10,000,000 | 4.30 | 4.73 | 6.91 | 4.31 |
| uspan:1.3e12 | 10,000,000 | 4.35 | 4.71 | 6.87 | 5.57 |
