# Competitor cache on the Apple M5 Pro

[competitors.csv](competitors.csv) contains one observation for each of 170 cells and five external sorts. Times are in nanoseconds for the entire array. Divide by the number of keys for ns/key. These measurements use the pinned competitors, GCC 16.2 and `-O3 -mcpu=native`.

| Cache status | Entries | Meaning |
|---|---:|---|
| measured | 829 | Median of completed, validated timings. For an even number of repetitions, average the two middle observations. |
| reported_upper_median | 15 | The interrupted process had printed its upper middle timing to two decimal places in ns/key for these ten-repetition cells. |
| reported_median | 5 | The same process printed the middle of five timings to two decimal places in ns/key. |
| right_censored | 1 | The exact median is unavailable. `lower_bound_ns` records a measured lower bound. |

The first 133 complete cells came from the earlier ten-repetition sweep. Each repetition there regenerated the input with the same advancing random generator. The remaining 37 use one fixed seed-42 input per cell and the 10/5/3 repetition schedule. The twenty recovered console medians cover all five competitors on 10K, 100K, 1M and 10M heavy-tailed keys. Reusing them avoids running those sorts again.

## BLS on 100M heavy-tailed keys

The original batch spent over seven minutes in the process for this distribution. A process sample located the long-running sort in BLS’s final insertion loop. A diagnostic copy stopped immediately before that loop on the same input. An independent merge-and-count pass found **1,313,327,720,427,746 inversions** still present. The insertion implementation in the pinned BLS moves one element for each inversion.

The fastest completed competitor was IPS4o at **0.901184 seconds**. The slowest of the other four was std::sort at **4.388631 seconds**. Three fresh runs of unmodified BLS each reached a 4.390-second process-CPU timer set immediately before the sort. The cache records a conservative **4.389-second lower bound** and leaves the median empty. BLS could not change the per-cell winner. The lower bound does not enter a geometric mean as a measured time.
