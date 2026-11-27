# nfs-classic benchmark

Apple M5 Pro, GCC 16.2, C++20 with `-O3 -mcpu=native`. All 1,292 timed classic outputs and all 1,292 paired original outputs passed.

| Implementation | Original, all 170 | Original, 105 without patterns | Best competitor, all 170 | Best competitor, 105 without patterns |
|---|---:|---:|---:|---:|
| classic | 1.904× | 1.576× | 2.094× | 1.674× |

Radix, classic and the original were timed in the same process on the same seed-42 input in each cell. Input generation, copying and validation were outside the timer. The order rotated across ten repetitions at 10K, 100K and 1M, five at 10M and three at 100M. Each ratio divides median times and each cell has equal weight.

The best external sort is the minimum cached median among BLS, PDQSort, IPS4o, Spreadsort and std::sort for that cell. The cache reuses 133 complete ten-trial cells from the earlier rotating sweep, which generated a fresh input on each trial. The remaining 37 cells use fixed inputs. Twenty heavy-tail competitor medians at 10K through 10M were recovered from the benchmark’s two-decimal console output. For ten repetitions, the console reported the upper middle observation. The cache labels them accordingly. External comparisons cross benchmark runs. Both branches’ comparisons with the original are paired. BLS exceeded 4.389 seconds in three bounded trials on 100M heavy-tailed keys. Its exact median is unavailable. IPS4o won that cell in 0.901 seconds. See [cache qualifications](competitors-notes.md).

The 65 excluded cells cover sorted and reverse inputs, both swap rates, all three organ pipes, all sorted-run inputs and all sorted-prefix tails. It retains duplicate counts, heavy tails, sentinels, both spans, signed normal f64 and all four SOSD files.

Timing started with `14:00  up 11 days,  2:44, 1 user, load averages: 5.43 3.89 3.11` and finished with `14:06  up 11 days,  2:50, 1 user, load averages: 3.08 3.75 3.39`.

[Raw candidate timings](nfs-classic-final.csv) · [Competitor cache](competitors.csv) · [Implementation notes](nfs-classic-notes.md)

## Speedup over the original

| Input | 10K | 100K | 1M | 10M | 100M |
|---|---:|---:|---:|---:|
| uniform | 1.36× | 1.66× | 1.54× | 1.44× | 1.44× |
| sorted | 2.00× | 2.36× | 2.08× | 2.00× | 2.13× |
| reverse | 2.00× | 1.58× | 1.54× | 1.39× | 1.45× |
| organpipe | 21.50× | 30.42× | 29.79× | 27.59× | 34.96× |
| normal | 1.32× | 1.71× | 1.56× | 1.43× | 1.45× |
| nearlysorted | 1.13× | 1.19× | 1.18× | 1.16× | 1.04× |
| fewunique | 1.25× | 1.18× | 1.17× | 1.19× | 1.17× |
| exponential | 1.15× | 1.71× | 1.56× | 1.47× | 1.49× |
| clustered | 1.45× | 1.09× | 1.51× | 1.49× | 1.38× |
| sosd_lognormal | 1.28× | 1.49× | 1.54× | 1.50× | 1.39× |
| sosd_normal | 1.35× | 1.69× | 1.56× | 1.44× | 1.44× |
| sosd_dense | 1.29× | 1.51× | 1.42× | 1.32× | 1.37× |
| sosd_sparse | 1.45× | 1.67× | 1.59× | 1.42× | 1.44× |
| books | 1.34× | 1.72× | 1.56× | 1.41× | 1.33× |
| fb | 1.31× | 1.51× | 1.40× | 1.25× | 1.25× |
| osm | 1.89× | 1.31× | 1.33× | 1.31× | 1.30× |
| wiki | 1.27× | 1.75× | 1.60× | 1.41× | 1.12× |
| dup300 | 1.45× | 1.15× | 1.14× | 1.11× | 1.08× |
| dup1000 | 1.45× | 1.26× | 1.26× | 1.15× | 1.13× |
| dup10000 | 1.20× | 1.51× | 1.14× | 1.15× | 1.16× |
| organpipe_overlap | 1.73× | 2.90× | 2.76× | 2.69× | 2.67× |
| organpipe_mirror | 1.39× | 2.34× | 2.66× | 2.64× | 3.01× |
| runs2 | 1.81× | 2.96× | 2.67× | 2.56× | 2.65× |
| runs16 | 1.31× | 1.51× | 1.41× | 1.48× | 1.28× |
| runs1000keys | 1.29× | 1.50× | 1.39× | 1.40× | 1.45× |
| tail1 | 6.17× | 10.91× | 11.62× | 12.71× | 8.63× |
| tail10 | 4.75× | 5.23× | 5.00× | 3.99× | 3.87× |
| tail40 | 1.06× | 1.74× | 1.63× | 1.59× | 1.58× |
| nearlysorted5 | 1.13× | 1.22× | 1.15× | 1.12× | 1.06× |
| heavytail | 4.49× | 4.24× | 4.91× | 5.08× | 4.68× |
| sentinel | 4.65× | 6.19× | 6.56× | 6.25× | 6.62× |
| span1e19 | 1.44× | 1.69× | 1.60× | 1.59× | 1.43× |
| span1p3e12 | 1.30× | 1.68× | 1.57× | 1.60× | 1.49× |
| signednormal (f64) | 1.16× | 1.40× | 1.31× | 1.27× | 1.27× |

## Speedup over the fastest external sort

| Input | 10K | 100K | 1M | 10M | 100M |
|---|---:|---:|---:|---:|
| uniform | 3.27× | 3.22× | 2.99× | 1.34× | 1.05× |
| sorted | 1.33× | 2.00× | 1.87× | 1.79× | 1.98× |
| reverse | 1.50× | 1.35× | 1.51× | 1.20× | 1.45× |
| organpipe | 13.00× | 10.90× | 14.84× | 12.36× | 15.68× |
| normal | 2.41× | 3.42× | 3.08× | 1.23× | 1.31× |
| nearlysorted | 1.05× | 0.81× | 0.99× | 0.86× | 0.80× |
| fewunique | 1.23× | 0.51× | 1.03× | 1.09× | 1.27× |
| exponential | 2.78× | 3.43× | 2.84× | 1.25× | 1.34× |
| clustered | 2.52× | 0.90× | 1.23× | 1.20× | 1.30× |
| sosd_lognormal | 2.04× | 2.15× | 1.41× | 1.30× | 1.59× |
| sosd_normal | 2.50× | 3.28× | 3.06× | 1.24× | 1.31× |
| sosd_dense | 2.93× | 3.20× | 3.49× | 1.28× | 1.12× |
| sosd_sparse | 3.36× | 3.38× | 3.58× | 1.40× | 1.19× |
| books | 3.20× | 3.52× | 3.02× | 1.21× | 1.16× |
| fb | 3.05× | 2.71× | 2.54× | 1.14× | 1.15× |
| osm | 2.14× | 1.59× | 1.55× | 1.10× | 1.22× |
| wiki | 2.71× | 3.12× | 2.98× | 1.20× | 0.97× |
| dup300 | 2.36× | 0.55× | 0.62× | 0.91× | 1.23× |
| dup1000 | 2.89× | 1.25× | 0.85× | 1.04× | 1.38× |
| dup10000 | 3.12× | 2.68× | 1.64× | 0.68× | 1.19× |
| organpipe_overlap | 4.11× | 4.37× | 4.95× | 2.72× | 2.42× |
| organpipe_mirror | 3.04× | 3.60× | 4.68× | 2.99× | 2.73× |
| runs2 | 4.17× | 4.62× | 4.64× | 3.13× | 2.66× |
| runs16 | 3.54× | 2.72× | 2.86× | 1.37× | 1.09× |
| runs1000keys | 3.08× | 2.80× | 2.83× | 1.36× | 1.22× |
| tail1 | 11.83× | 14.97× | 17.12× | 16.98× | 9.72× |
| tail10 | 9.31× | 7.59× | 8.05× | 5.21× | 4.17× |
| tail40 | 2.74× | 3.16× | 3.53× | 1.94× | 1.54× |
| nearlysorted5 | 1.29× | 1.25× | 1.48× | 1.23× | 1.07× |
| heavytail | 1.41× | 0.64× | 0.69× | 0.68× | 0.88× |
| sentinel | 1.05× | 0.89× | 1.06× | 0.85× | 0.97× |
| span1e19 | 3.11× | 3.93× | 3.71× | 1.43× | 1.29× |
| span1p3e12 | 2.43× | 3.15× | 3.62× | 1.40× | 1.25× |
| signednormal (f64) | 3.48× | 3.20× | 2.63× | 1.11× | 1.16× |

## Worst cells

| Reference | Input | Implementation ns/key | Reference ns/key | Speedup |
|---|---|---:|---:|---:|
| nfss_base | nearlysorted u64, 100M | 8.143 | 8.506 | 1.045× |
| nfss_base | tail40 u64, 10K | 3.300 | 3.500 | 1.061× |
| nfss_base | nearlysorted5 u64, 100M | 8.161 | 8.659 | 1.061× |
| nfss_base | dup300 u64, 100M | 2.428 | 2.620 | 1.079× |
| nfss_base | clustered u64, 100K | 9.990 | 10.865 | 1.088× |
| bls | fewunique u64, 100K | 4.880 | 2.500 | 0.512× |
| bls | dup300 u64, 100K | 5.655 | 3.090 | 0.546× |
| bls | dup300 u64, 1M | 4.191 | 2.603 | 0.621× |
| bls | heavytail u64, 100K | 10.255 | 6.540 | 0.638× |
| bls | dup10000 u64, 10M | 7.230 | 4.897 | 0.677× |

## Regressions over 3%

Every cell stayed within 3% of the original.
