#pragma once

/**
 Copyright 2026 Fran Sučić, Leo Vitasović, Nikola Petrušić

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 *
 */


#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef NFS_CLASSIC_CACHE_LINE_BYTES
#define NFS_CLASSIC_CACHE_LINE_BYTES 64
#endif
#ifndef NFS_CLASSIC_L1D_BYTES
#define NFS_CLASSIC_L1D_BYTES (64 * 1024)
#endif
#ifndef NFS_CLASSIC_L2_BYTES
#define NFS_CLASSIC_L2_BYTES (2 * 1024 * 1024)
#endif

namespace nfs {
    /*
     * Optimised classic NFS Sort (keeps the paper's design from arXiv:2607.11850
     * and the O(log n) extra-memory bound). Main changes from the paper's header:
     *
     * - Tail peeling: a 64-key sample splits off extreme outliers before bucketing
     *   so a few huge or tiny keys don't squash the main range into one bucket.
     * - Narrow integer leaves: two stable digit scatters finish ranges whose span
     *   splits into two cache-sized tables; singleton intervals use counting sort.
     * - Sparse-placement leaf: large dispersed children finish in an open-addressed
     *   slot table and occupancy bitmap instead of another fragment pass.
     * - Four-lane min/max: scans exact bounds across four independent lanes so the
     *   CPU can overlap comparisons.
     * - Cache-derived constants: fragment, bucket, and leaf limits come from L1/L2
     *   cache budgets. Override at compile time with NFS_CLASSIC_CACHE_LINE_BYTES,
     *   NFS_CLASSIC_L1D_BYTES, and NFS_CLASSIC_L2_BYTES (keep them identical across
     *   translation units).
     * - Natural-run merges: detects up to 4 long runs (or a sorted prefix/suffix
     *   covering at least half the input) and merges with a buffer capped at L2
     *   size, rotating recursively when both sides exceed it.
     * - Exact integer interval arithmetic: 128-bit multiply-high (with division
     *   fallback) maps keys and inverts bucket boundaries without double precision
     *   loss, using size_t indices throughout.
     */

    namespace internal {
        template<typename T>
        struct Cache;

        template<typename T>
        static void insertion_sort(T *first, T *last);

        template<typename T, bool WholeLeaf = false>
        static void general_sort(T *first, T *last);

        template<typename T, typename Sort>
        static bool sort_runs(T *first, T *last, Sort sort_remainder);
    }

    /** Sort a contiguous arithmetic range in ascending order, placing floating-point NaNs last. */
    template<typename It>
    void sort(It begin, It end) {
        using T = typename std::iterator_traits<It>::value_type;
        static_assert(std::is_arithmetic_v<T> && std::contiguous_iterator<It>);

        if (begin == end) return;

        T *first = std::to_address(begin);
        T *last = first + (end - begin);

        if constexpr (std::is_floating_point_v<T>) {
            auto is_number = [](T x) { return !std::isnan(x); };
            T *first_nan = std::find_if_not(first, last, is_number);
            if (first_nan != last) last = std::partition(first_nan, last, is_number);
        }

        if (last - first < 2) return;

        if constexpr (std::is_same_v<T, bool>) {
            std::partition(first, last, [](bool x) { return !x; });
        } else {
            if (last - first < 32) {
                internal::insertion_sort(first, last);
            } else if (!internal::sort_runs(first, last, internal::general_sort<T>)) {
                constexpr auto leaf_limit = internal::Cache<T>::histogram_limit;
                const auto n = std::size_t(last - first);
                // Whole arrays up to two histogram leaves skip fragment setup to save an extra pass.
                if (n > leaf_limit && n <= 2 * leaf_limit) {
                    internal::general_sort<T, true>(first, last);
                } else {
                    internal::general_sort(first, last);
                }
            }
        }
    }

    namespace internal {
        // Size fragments, buckets, and leaves from L1/L2 budgets instead of hardcoded constants.
        template<typename T>
        struct Cache {
            static constexpr std::size_t line_bytes = NFS_CLASSIC_CACHE_LINE_BYTES;
            static constexpr std::size_t l1_bytes = NFS_CLASSIC_L1D_BYTES;
            static constexpr std::size_t l2_bytes = NFS_CLASSIC_L2_BYTES;
            static_assert(std::has_single_bit(line_bytes) && line_bytes >= sizeof(T));

            static constexpr std::size_t fragment_bytes = 8 * line_bytes;
            static constexpr unsigned fragment_keys = fragment_bytes / sizeof(T);
            static constexpr std::size_t bucket_bytes = 2 * sizeof(std::size_t) + 2 * sizeof(unsigned);
            static constexpr unsigned max_buckets = std::bit_floor(
                std::min(l1_bytes / bucket_bytes, l2_bytes / (2 * fragment_bytes)));
            static_assert(max_buckets >= 2);

            static constexpr unsigned slots_per_key = 4;
            static constexpr std::size_t placement_limit = std::bit_floor(l2_bytes / (slots_per_key * sizeof(T)));
            static_assert(placement_limit <= std::numeric_limits<unsigned>::max() / (2 * slots_per_key));

            static constexpr std::size_t histogram_limit = std::bit_floor(std::min(
                placement_limit, (l2_bytes / 8) / (2 * sizeof(T) + 2 * sizeof(unsigned))));
            static constexpr std::size_t histogram_bins = std::min(
                histogram_limit, l1_bytes / (sizeof(std::size_t) + sizeof(unsigned)));
            static constexpr unsigned digit_bins = std::bit_floor(
                std::min(placement_limit, l2_bytes / (16 * sizeof(unsigned))));
            static constexpr unsigned digit_bins_per_key = std::max<std::size_t>(1, line_bytes / (4 * sizeof(unsigned)));
            static constexpr unsigned min_buckets = std::bit_ceil(unsigned(std::clamp<std::size_t>(
                histogram_bins / fragment_keys, 1, max_buckets)));
            static_assert(histogram_limit >= 32);

            static unsigned fanout(std::size_t n) {
                const std::size_t target = histogram_limit / 2;
                const std::size_t count = std::min<std::size_t>(max_buckets, 1 + (n - 1) / target);
                return std::max(min_buckets, unsigned(std::bit_ceil(count)));
            }
        };

        template<typename T>
        inline unsigned placement_slots(std::size_t n) {
            return Cache<T>::slots_per_key * std::bit_ceil(unsigned(n));
        }

        // Exact integer mapping via 128-bit multiply-high (division fallback) avoids double precision loss on 64-bit keys.
        template<typename T>
        struct Intervals {
            T low, high;
            unsigned bins;
            std::uint64_t step = 0, reciprocal = 0;
            double scale = 0;

            Intervals(T lo, T hi, unsigned count) : low(lo), high(hi), bins(count) {
                if constexpr (std::is_integral_v<T>) {
                    step = distance(high) / bins + 1;
#if defined(__SIZEOF_INT128__)
                    if (step > 1) {
                        reciprocal = std::uint64_t((__uint128_t(bins) << 64) / (__uint128_t(distance(high)) + 1));
                    }
#endif
                } else {
                    scale = (bins - 1) / (double(high) - double(low));
                }
            }

            std::uint64_t distance(T x) const requires std::is_integral_v<T> {
                using U = std::make_unsigned_t<T>;
                return U(U(x) - U(low));
            }

            unsigned index(T x) const {
                if constexpr (std::is_integral_v<T>) {
                    const auto d = distance(x);
                    if (step == 1) return unsigned(d);
#if defined(__SIZEOF_INT128__)
                    return unsigned((__uint128_t(d) * reciprocal) >> 64);
#else
                    return unsigned(d / step);
#endif
                } else {
                    const double pos = (double(x) - double(low)) * scale;
                    return unsigned(std::clamp(pos, 0.0, double(bins - 1)));
                }
            }

            std::pair<T, T> bounds(unsigned b) const {
                if constexpr (std::is_integral_v<T>) {
                    using U = std::make_unsigned_t<T>;
                    auto value = [&](std::uint64_t offset) { return T(U(U(low) + U(offset))); };
#if defined(__SIZEOF_INT128__)
                    if (step > 1) {
                        auto boundary = [&](unsigned i) {
                            return ((__uint128_t(i) << 64) + reciprocal - 1) / reciprocal;
                        };
                        return {
                            value(std::uint64_t(boundary(b))),
                            value(std::uint64_t(std::min<__uint128_t>(distance(high), boundary(b + 1) - 1)))
                        };
                    }
#endif
                    const auto begin = std::uint64_t(b) * step;
                    return {value(begin), value(begin + std::min(step - 1, distance(high) - begin))};
                } else {
                    const auto boundary = [&](unsigned i) {
                        return T(std::clamp(double(low) + i / scale, double(low), double(high)));
                    };
                    return {b ? boundary(b) : low, b + 1 == bins ? high : boundary(b + 1)};
                }
            }
        };

        // Scratch buffers allocated once per sort; items and aux_offsets are reused across scatter and leaf passes.
        template<typename T>
        struct Workspace {
            using C = Cache<T>;

            explicit Workspace(std::size_t n, bool whole_leaf) {
                const auto bins = !whole_leaf && n > C::histogram_limit ? C::fanout(n) + 2 : 0;
                const auto leaf_bins = std::min(C::histogram_bins, n);
                std::size_t item_count = n;
                if (bins) {
                    const auto slots = placement_slots<T>(std::min(n, C::placement_limit));
                    item_count = std::max<std::size_t>(std::size_t(bins) * C::fragment_keys, slots + 64);
                    bitmap = std::make_unique_for_overwrite<std::uint64_t[]>(slots / 64 + 1);
                }
                items = std::make_unique_for_overwrite<T[]>(item_count);
                bucket_sizes = std::make_unique_for_overwrite<std::size_t[]>(std::max<std::size_t>(bins, leaf_bins));
                const auto digit_slots = C::digit_bins_per_key * std::min<std::size_t>(n, C::digit_bins);
                const auto index_bins = std::max(leaf_bins, std::min<std::size_t>(C::digit_bins, digit_slots));
                offsets = std::make_unique_for_overwrite<unsigned[]>(index_bins);
                aux_offsets = std::make_unique_for_overwrite<unsigned[]>(index_bins);
                bucket_ends = std::make_unique_for_overwrite<std::size_t[]>(bins);
                bucket_write_off = std::make_unique_for_overwrite<std::size_t[]>(bins);
                fragment_sizes = std::make_unique_for_overwrite<unsigned[]>(bins);
            }

            std::unique_ptr<T[]> items;
            std::unique_ptr<std::uint64_t[]> bitmap;
            std::array<T, C::fragment_keys> tmp;
            std::unique_ptr<std::size_t[]> bucket_sizes, bucket_ends, bucket_write_off;
            std::unique_ptr<unsigned[]> offsets, aux_offsets, fragment_sizes;
        };

        // Forward declarations so the sorting pipeline below reads top-down
        template<typename T>
        static bool peel_tails(T *data, std::size_t n, T low, T high, Workspace<T> &ws);

        template<bool Sampled, bool WholeLeaf = false, typename T>
        static void nfss_core(T *src, std::size_t n, T low, T high, unsigned stagnant_passes, Workspace<T> &ws);

        template<bool Sampled, typename T>
        static std::vector<std::size_t> fragment_scatter(
            T *src, std::size_t end, const Intervals<T> &intervals,
            Workspace<T> &ws, T &observed_min, T &observed_max);

        template<typename T>
        static std::vector<std::size_t> naive_scatter(
            T *src, std::size_t n, const Intervals<T> &intervals, Workspace<T> &ws);

        template<typename T>
        static bool placement_leaf(T *data, std::size_t n, T low, T high, Workspace<T> &ws);

        template<typename T>
        static bool digit_leaf(T *data, std::size_t n, const Intervals<T> &intervals, Workspace<T> &ws);

        template<typename T>
        static void merge_runs(T *first, T *middle, T *last);

        template<bool descending, typename T>
        static T *run_end(T *first, T *last);

        template<typename T>
        static std::pair<T, T> extrema(const T *data, std::size_t n);

        template<typename T>
        static bool homogeneous(const T *data, std::size_t n);

        template<typename T>
        static void swap_fragment(T *a, T *b, int cap, T *tmp);

        template<typename T, bool WholeLeaf>
        static void general_sort(T *first, T *last) {
            const std::size_t n = last - first;
            if (n <= 32) {
                if (n > 1) insertion_sort(first, last);
                return;
            }

            Workspace<T> ws(n, WholeLeaf);
            T low = first[0], high = first[0];

            if (!WholeLeaf && n > Cache<T>::histogram_limit) {
                // Jittered stratified sample so periodic run lengths don't put every sample on a boundary.
                const std::size_t samples = std::max<std::size_t>(1000, n / 1000);
                const std::size_t stride = n / samples;
                std::uint32_t rng_state = 42;
                for (std::size_t i = 0; i < samples; ++i) {
                    rng_state = rng_state * 1664525u + 1013904223u;
                    const auto index = i * stride + (std::uint64_t(rng_state) * stride >> 32);
                    low = std::min(low, first[index]);
                    high = std::max(high, first[index]);
                }

                if (low != high) {
                    if (!peel_tails(first, n, low, high, ws)) {
                        nfss_core<true>(first, n, low, high, 0, ws);
                    }
                    insertion_sort(first, last);
                    return;
                }
            }

            const auto [exact_low, exact_high] = extrema(first, n);
            if (exact_low == exact_high) return;
            nfss_core<false, WholeLeaf>(first, n, exact_low, exact_high, 0, ws);
            insertion_sort(first, last);
        }

        // Peel extreme tails first so a few outliers don't squash the bucket range.
        template<typename T>
        static bool peel_tails(T *data, std::size_t n, T low, T high, Workspace<T> &ws) {
            std::array<T, 64> sample;
            const std::size_t stride = n / sample.size();
            std::uint32_t rng_state = 73;
            for (unsigned i = 0; i < sample.size(); ++i) {
                rng_state = rng_state * 1664525u + 1013904223u;
                sample[i] = data[i * stride + (std::uint64_t(rng_state) * stride >> 32)];
            }
            std::sort(sample.begin(), sample.end());

            const T inner_low = sample[sample.size() / 8];
            const T inner_high = sample[sample.size() - sample.size() / 8 - 1];
            auto distance = [](T a, T b) {
                if constexpr (std::is_integral_v<T>) {
                    using U = std::make_unsigned_t<T>;
                    return U(U(b) - U(a));
                } else {
                    return b - a;
                }
            };

            const auto spread = distance(inner_low, inner_high);
            const unsigned bins = Cache<T>::fanout(n);
            const bool has_lower_tail = low < inner_low && distance(low, inner_low) / bins > spread;
            const bool has_upper_tail = high > inner_high && distance(inner_high, high) / bins > spread;
            if (!has_lower_tail && !has_upper_tail) return false;

            T *mid_start = data;
            T *mid_end = data + n;
            if (has_lower_tail) {
                const T cut = T(inner_low - spread);
                mid_start = std::partition(mid_start, mid_end, [=](T x) { return x < cut; });
            }
            if (has_upper_tail) {
                const T cut = T(inner_high + spread);
                mid_end = std::partition(mid_start, mid_end, [=](T x) { return x <= cut; });
            }

            auto sort_part = [&](T *part_first, T *part_last) {
                if (part_last - part_first <= 16) return;
                auto [part_low, part_high] = extrema(part_first, part_last - part_first);
                if (part_low != part_high) {
                    nfss_core<false>(part_first, part_last - part_first, part_low, part_high, 0, ws);
                }
            };
            sort_part(data, mid_start);
            sort_part(mid_start, mid_end);
            sort_part(mid_end, data + n);
            return true;
        }

        template<bool Sampled, bool WholeLeaf, typename T>
        static void nfss_core(T *src, std::size_t n, T low, T high,
                              unsigned stagnant_passes, Workspace<T> &ws) {
            if (n <= 16 || homogeneous(src, n)) return;

            const bool is_leaf = WholeLeaf || n <= Cache<T>::histogram_limit;
            const unsigned bins = is_leaf ? unsigned(std::min(Cache<T>::histogram_bins, n)) : Cache<T>::fanout(n);
            Intervals<T> intervals(low, high, bins);

            if constexpr (std::is_floating_point_v<T>) {
                if (!(intervals.scale > 0 && std::isfinite(intervals.scale))) {
                    std::sort(src, src + n);
                    return;
                }
            }

            if constexpr (std::is_integral_v<T> && !Sampled) {
                // Each integer in [low, high] has its own bucket, so emit directly from counts.
                if (intervals.step == 1) {
                    using U = std::make_unsigned_t<T>;
                    auto counts = ws.bucket_sizes.get();
                    const auto values = unsigned(intervals.distance(high)) + 1;
                    std::fill_n(counts, values, 0);
                    for (std::size_t i = 0; i < n; ++i) ++counts[intervals.distance(src[i])];
                    for (unsigned b = 0; b < values; ++b) {
                        src = std::fill_n(src, counts[b], T(U(U(low) + U(b))));
                    }
                    return;
                }
                // Narrow integer ranges finish in two stable digit passes instead of recursing.
                if (n <= Cache<T>::placement_limit && digit_leaf(src, n, intervals, ws)) return;
            }

            if constexpr (!Sampled) {
                // Finish large dispersed children in one sparse-placement pass instead of another fragment scatter.
                if (!is_leaf && n <= Cache<T>::placement_limit && placement_leaf(src, n, low, high, ws)) return;
            }

            T observed_low = low;
            T observed_high = high;
            auto offsets = is_leaf
                ? naive_scatter(src, n, intervals, ws)
                : fragment_scatter<Sampled>(src, n, intervals, ws, observed_low, observed_high);
            if (offsets.empty()) return;

            auto recurse = [&](unsigned bucket, T lo, T hi) {
                T *child = src + offsets[bucket];
                const std::size_t size = offsets[bucket + 1] - offsets[bucket];
                if (size <= 16) return;

                // Stalled child (>95% of parent): remeasure exact bounds; fall back to std::sort after two stalls.
                if (size > n - n / 20) {
                    if (homogeneous(child, size)) return;
                    if (stagnant_passes >= 2) {
                        std::sort(child, child + size);
                        return;
                    }
                    auto [child_low, child_high] = extrema(child, size);
                    nfss_core<false, WholeLeaf>(child, size, child_low, child_high, stagnant_passes + 1, ws);
                } else {
                    nfss_core<false, WholeLeaf>(child, size, lo, hi, 0, ws);
                }
            };

            if constexpr (Sampled) {
                recurse(0, observed_low, low);
                recurse(bins + 1, high, observed_high);
            }
            for (unsigned b = 0; b < bins; ++b) {
                const unsigned bucket_idx = b + unsigned(Sampled);
                if (offsets[bucket_idx + 1] - offsets[bucket_idx] <= 16) continue;
                auto [lo, hi] = intervals.bounds(b);
                recurse(bucket_idx, lo, hi);
            }
        }

        // IPS4o-inspired bucketed fragment scatter
        template<bool Sampled, typename T>
        static std::vector<std::size_t> fragment_scatter(
            T *src,
            std::size_t end,
            const Intervals<T> &intervals,
            Workspace<T> &ws,
            T &observed_min,
            T &observed_max
        ) {
            constexpr int CAP = Cache<T>::fragment_keys;
            const unsigned base_bucket_number = intervals.bins;
            const unsigned bucket_number = base_bucket_number + (Sampled ? 2 : 0);

            auto *helper = ws.items.get();
            auto *bucket_end_offset = ws.bucket_ends.get();
            auto *bucket_write_off = ws.bucket_write_off.get();
            auto *bucket_sizes = ws.bucket_sizes.get();
            auto *fragment_sizes = ws.fragment_sizes.get();

            auto classify = [&](T x) -> unsigned {
                if constexpr (Sampled) {
                    if (x < intervals.low) {
                        observed_min = std::min(observed_min, x);
                        return 0;
                    }
                    if (x > intervals.high) {
                        observed_max = std::max(observed_max, x);
                        return base_bucket_number + 1;
                    }
                }
                return intervals.index(x) + unsigned(Sampled);
            };

            // Coordinates of our helper array based on cap size
            auto frag = [&](const unsigned b) -> T * {
                return helper + static_cast<size_t>(b) * CAP;
            };

            // init helper variables
            std::fill_n(bucket_sizes, bucket_number, 0);
            std::fill_n(fragment_sizes, bucket_number, 0);
            std::size_t write_pos = 0;
            std::size_t fragments_written = 0;

            // ------------------------
            // FLUSH FRAGMENTS INTO SRC
            // ------------------------
            // Count whole fragments on flush and add partial tails once afterward to keep the inner loop lean.
            for (std::size_t i = 0; i < end; ++i) {
                const unsigned index = classify(src[i]);
                const int k = fragment_sizes[index]++;

                frag(index)[k] = src[i];

                if (k + 1 == CAP) {
                    std::memcpy(src + write_pos, frag(index), CAP * sizeof(T));
                    bucket_sizes[index] += CAP;
                    fragments_written++;
                    write_pos += CAP;
                    fragment_sizes[index] = 0;
                }
            }

            // calculate bucket end offsets
            for (unsigned b = 0; b < bucket_number; ++b) {
                bucket_sizes[b] += fragment_sizes[b];
            }
            bucket_end_offset[0] = bucket_sizes[0];
            for (unsigned b = 1; b < bucket_number; ++b) {
                bucket_end_offset[b] = bucket_end_offset[b - 1] + bucket_sizes[b];
            }

            // calculate CAP aligned write offsets
            bucket_write_off[0] = 0;
            for (unsigned b = 1; b < bucket_number; ++b) {
                const std::size_t prev_end = bucket_end_offset[b - 1];
                std::size_t aligned = ((prev_end + CAP - 1) / CAP) * CAP; // ceil(prev_end/CAP)*CAP
                if (aligned > bucket_end_offset[b]) aligned = bucket_end_offset[b];
                bucket_write_off[b] = aligned;
            }

            // cap-sized fragment helper-function for src
            auto frag_ptr = [&](const std::size_t frag_idx) -> T * {
                return src + frag_idx * CAP;
            };

            // ------------------------
            // DEFRAGMENTATION BY MOVING FRAGMENTS AND PUTTING THEM IN THE RIGHT PLACE
            // ALSO PUTTING SPILLOVER INTO HELPER
            // ------------------------

            unsigned stored_bucket_idx = 0;

            for (std::size_t fragment_idx = 0; fragment_idx < fragments_written; ++fragment_idx) {
                const std::size_t cur_fragment_start_off = fragment_idx * CAP;

                while (stored_bucket_idx < bucket_number - 1 &&
                       cur_fragment_start_off >= bucket_end_offset[stored_bucket_idx]) {
                    ++stored_bucket_idx;
                }

                if (cur_fragment_start_off < bucket_write_off[stored_bucket_idx]) {
                    continue;
                }

                const unsigned owner_bucket = classify(src[cur_fragment_start_off]);
                const std::size_t dest_off = bucket_write_off[owner_bucket];

                if (dest_off < cur_fragment_start_off ||
                    dest_off >= fragments_written * CAP) {
                    if (dest_off + CAP <= bucket_end_offset[owner_bucket]) {
                        std::copy(src + cur_fragment_start_off,
                                  src + cur_fragment_start_off + CAP,
                                  src + dest_off);

                        bucket_write_off[owner_bucket] += CAP;
                    } else {
                        const int fit = bucket_end_offset[owner_bucket] - dest_off; // 0..CAP-1

                        std::copy(src + cur_fragment_start_off,
                                  src + cur_fragment_start_off + fit,
                                  src + dest_off);

                        bucket_write_off[owner_bucket] = bucket_end_offset[owner_bucket];

                        for (int t = fit; t < CAP; ++t) {
                            frag(owner_bucket)[fragment_sizes[owner_bucket]++] =
                                *(src + cur_fragment_start_off + t);
                        }
                    }
                } else {
                    if (owner_bucket == stored_bucket_idx) {
                        const std::size_t remaining = bucket_end_offset[stored_bucket_idx] -
                                                      bucket_write_off[stored_bucket_idx];

                        if (remaining < CAP) {
                            std::copy_backward(
                                src + cur_fragment_start_off,
                                src + cur_fragment_start_off + remaining,
                                src + bucket_write_off[stored_bucket_idx] + remaining
                            );

                            for (int t = remaining; t < CAP; ++t) {
                                frag(stored_bucket_idx)[fragment_sizes[stored_bucket_idx]++] =
                                    *(src + cur_fragment_start_off + t);
                            }

                            bucket_write_off[stored_bucket_idx] = bucket_end_offset[stored_bucket_idx];
                        } else {
                            if (bucket_write_off[stored_bucket_idx] != cur_fragment_start_off) {
                                std::copy(src + cur_fragment_start_off,
                                          src + cur_fragment_start_off + CAP,
                                          src + bucket_write_off[stored_bucket_idx]);
                            }
                            bucket_write_off[stored_bucket_idx] += CAP;
                        }
                    } else {
                        const std::size_t dest_frag_idx = dest_off / CAP;
                        const unsigned incumbent_owner = classify(src[bucket_write_off[owner_bucket]]);

                        if (incumbent_owner != owner_bucket) {
                            swap_fragment<T>(frag_ptr(dest_frag_idx), frag_ptr(fragment_idx), CAP, ws.tmp.data());
                            --fragment_idx;
                        } else {
                            bucket_write_off[owner_bucket] += CAP;
                            --fragment_idx;
                        }
                    }
                }
            }

            // copy bucket offsets for return value
            std::vector<std::size_t> bucket_offsets(bucket_number + 1);
            bucket_offsets[0] = 0;
            for (unsigned b = 1; b < bucket_number + 1; ++b) {
                bucket_offsets[b] = bucket_end_offset[b - 1];
            }

            // ------------------------
            // FILL UP WITH REST (SPILLOVER)
            // ------------------------
            for (unsigned b = 0; b < bucket_number; ++b) {
                const int k = fragment_sizes[b];
                if (k == 0) continue;

                std::size_t write_off = bucket_offsets[b];

                int elm = 0;
                while (elm < k && (write_off % CAP) != 0) {
                    src[write_off++] = frag(b)[elm++];
                }

                const std::size_t tail_start = bucket_offsets[b + 1] - (k - elm);
                std::memcpy(src + tail_start, frag(b) + elm, static_cast<size_t>(k - elm) * sizeof(T));
            }

            return bucket_offsets;
        }

        template<typename T>
        static std::vector<std::size_t> naive_scatter(
            T *src, std::size_t n, const Intervals<T> &intervals, Workspace<T> &ws
        ) {
            const auto bins = intervals.bins;
            auto *counts = ws.bucket_sizes.get();
            auto *offsets = ws.offsets.get();
            auto *large_buckets = ws.aux_offsets.get();
            T *out = ws.items.get();

            std::fill_n(counts, bins, 0);
            for (std::size_t i = 0; i < n; ++i) ++counts[intervals.index(src[i])];

            unsigned sum = 0;
            unsigned crowded_keys = 0;
            unsigned num_large = 0;
            // Sum all crowded buckets so one large bucket doesn't hide others from the recursion threshold.
            for (unsigned b = 0; b < bins; ++b) {
                const unsigned c = unsigned(counts[b]);
                offsets[b] = sum;
                sum += c;
                crowded_keys += c >= 2 * Cache<T>::fragment_keys ? c : 0;
                large_buckets[num_large] = b;
                num_large += c >= 3;
            }

            for (std::size_t i = 0; i < n; ++i) {
                out[offsets[intervals.index(src[i])]++] = src[i];
            }

            if (crowded_keys >= Cache<T>::histogram_limit / 20) {
                std::copy_n(out, n, src);
                std::vector<std::size_t> ends(bins + 1);
                std::copy_n(offsets, bins, ends.begin() + 1);
                return ends;
            }

            // If the scattered output is nearly ordered, skip local sorting and let the final insertion pass finish.
            if (n >= 512) {
                unsigned inversions = 0;
                const std::size_t stride = (n - 1) / 64;
                for (unsigned i = 0; i < 64; ++i) {
                    inversions += out[i * stride + 1] < out[i * stride];
                }
                if (inversions <= 4) {
                    std::copy_n(out, n, src);
                    return {};
                }
            }

            // Pre-sort buckets of 3+ keys, then fix 2-key buckets with a branchless adjacent-pair copy.
            auto exchange = [](T &a, T &b) {
                const T x = a, y = b;
                const bool ordered = x < y;
                a = ordered ? x : y;
                b = ordered ? y : x;
            };
            for (unsigned i = 0; i < num_large; ++i) {
                const unsigned b = large_buckets[i];
                const unsigned c = unsigned(counts[b]);
                T *p = out + offsets[b] - c;
                if (c == 3) {
                    exchange(p[0], p[1]);
                    exchange(p[1], p[2]);
                    exchange(p[0], p[1]);
                } else if (c == 4) {
                    exchange(p[0], p[1]);
                    exchange(p[2], p[3]);
                    exchange(p[0], p[2]);
                    exchange(p[1], p[3]);
                    exchange(p[1], p[2]);
                } else {
                    insertion_sort(p, p + c);
                }
            }

            T carry = out[0];
            for (std::size_t i = 1; i < n; ++i) {
                const T next = out[i];
                const bool ordered = carry < next;
                src[i - 1] = ordered ? carry : next;
                carry = ordered ? next : carry;
            }
            src[n - 1] = carry;
            return {};
        }

        // Open-addressed sparse placement (4x slots + occupancy bitmap) finishes large dispersed leaves in one pass.
        template<typename T>
        static bool placement_leaf(T *data, std::size_t n, T low, T high, Workspace<T> &ws) {
            const unsigned slot_count = placement_slots<T>(n);
            const unsigned word_count = slot_count / 64 + 1;
            Intervals<T> positions(low, high, slot_count);

            if constexpr (std::is_integral_v<T>) {
                if (positions.distance(high) < slot_count) return false;
            } else if (!std::isfinite(positions.scale)) {
                return false;
            }

            int shift = 0;
            if constexpr (std::is_integral_v<T>) {
                shift = int(std::bit_width(positions.distance(high))) - int(std::bit_width(slot_count) - 1);
            }
            const auto position = [&](T value) {
                if constexpr (std::is_integral_v<T>) return unsigned(positions.distance(value) >> shift);
                else return positions.index(value);
            };

            // Reject nearly sorted or clustered leaves before touching the bitmap; data stays intact on fallback.
            unsigned descents = 0;
            std::uint64_t sample_slots = 0;
            for (unsigned j = 0; j < 16; ++j) {
                const auto i = j * (n / 16);
                descents += data[i + 1] < data[i];
                sample_slots |= UINT64_C(1) << (position(data[i]) % 64);
            }
            if (descents < 2 || std::popcount(sample_slots) < 8) return false;

            auto *occupied = ws.bitmap.get();
            auto *slots = ws.items.get();
            std::fill_n(occupied, word_count, UINT64_C(0));

            for (std::size_t i = 0; i < n; ++i) {
                const auto bin = position(data[i]);
                auto word = bin / 64;
                auto free_mask = ~occupied[word] & (~UINT64_C(0) << (bin % 64));
                if (!free_mask) {
                    free_mask = ~occupied[++word];
                    if (!free_mask) return false;
                }
                const auto bit = std::countr_zero(free_mask);
                occupied[word] |= UINT64_C(1) << bit;
                slots[word * 64 + bit] = data[i];
            }

            T *out = data;
            for (unsigned w = 0; w < word_count; ++w) {
                for (auto bits = occupied[w]; bits; bits &= bits - 1) {
                    const T x = slots[w * 64 + std::countr_zero(bits)];
                    T *pos = out++;
                    while (pos != data && x < pos[-1]) {
                        *pos = pos[-1];
                        --pos;
                    }
                    *pos = x;
                }
            }
            return true;
        }

        // Two stable LSD digit scatters on (x - low) when both count tables fit in the cache budget.
        template<typename T>
        static bool digit_leaf(T *data, std::size_t n, const Intervals<T> &intervals, Workspace<T> &ws) {
            const auto span = intervals.distance(intervals.high);
            const unsigned width = std::bit_width(span);
            const unsigned shift = width / 2;
            if (width == 0 || width > 2 * (std::bit_width(Cache<T>::digit_bins) - 1)) return false;

            const unsigned lower_size = 1u << shift;
            const unsigned upper_size = unsigned(span >> shift) + 1;
            if (std::max(lower_size, upper_size) > Cache<T>::digit_bins_per_key * n) return false;

            unsigned *lower_offsets = ws.offsets.get();
            unsigned *upper_offsets = ws.aux_offsets.get();
            std::fill_n(lower_offsets, lower_size, 0u);
            std::fill_n(upper_offsets, upper_size, 0u);

            const unsigned mask = lower_size - 1;
            for (std::size_t i = 0; i < n; ++i) {
                const auto key = intervals.distance(data[i]);
                ++lower_offsets[key & mask];
                ++upper_offsets[key >> shift];
            }
            std::exclusive_scan(lower_offsets, lower_offsets + lower_size, lower_offsets, 0u);
            std::exclusive_scan(upper_offsets, upper_offsets + upper_size, upper_offsets, 0u);

            T *out = ws.items.get();
            for (std::size_t i = 0; i < n; ++i) {
                out[lower_offsets[intervals.distance(data[i]) & mask]++] = data[i];
            }
            for (std::size_t i = 0; i < n; ++i) {
                data[upper_offsets[intervals.distance(out[i]) >> shift]++] = out[i];
            }
            return true;
        }

        // Ordered-input fast path: merge up to 4 long monotonic runs, or a sorted prefix/suffix of at least n/2.
        template<typename T, typename Sort>
        static bool sort_runs(T *first, T *last, Sort sort_remainder) {
            const auto n = last - first;
            T *bounds[5]{first};
            bool descending[4]{};
            T *p = first;
            unsigned runs = 0;

            while (p != last && runs < 4) {
                const bool is_desc = p + 1 != last && p[1] < p[0];
                T *end = is_desc ? run_end<true>(p, last) : run_end<false>(p, last);
                if (end != last && end - p < std::max<std::ptrdiff_t>(16, n / 16)) break;
                descending[runs] = is_desc;
                bounds[++runs] = end;
                p = end;
            }

            if (p != last) {
                if (runs && p - first >= n / 2 && runs < 4) {
                    sort_remainder(p, last);
                    bounds[++runs] = last;
                } else if (!runs && n >= 32) {
                    const bool is_desc = last[-1] < last[-2];
                    T *start = last - 2;
                    if (is_desc) {
                        while (start != first && start[-1] >= *start) --start;
                    } else {
                        while (start != first && start[-1] <= *start) --start;
                    }
                    if (last - start < n / 2) return false;
                    sort_remainder(first, start);
                    if (is_desc) std::reverse(start, last);
                    merge_runs(first, start, last);
                    return true;
                } else {
                    return false;
                }
            }

            for (unsigned i = 0; i < runs; ++i) {
                if (descending[i]) std::reverse(bounds[i], bounds[i + 1]);
            }
            for (unsigned width = 1; width < runs; width *= 2) {
                for (unsigned i = 0; i + width < runs; i += 2 * width) {
                    merge_runs(bounds[i], bounds[i + width], bounds[std::min(runs, i + 2 * width)]);
                }
            }
            return true;
        }

        template<typename T>
        static void merge_runs(T *first, T *middle, T *last) {
            if (middle == first || middle == last || middle[-1] <= *middle) return;

            first = std::upper_bound(first, middle, *middle);
            last = std::lower_bound(middle, last, middle[-1]);
            const auto left_len = middle - first;
            const auto right_len = last - middle;

            if (left_len == 1 || right_len == 1) {
                std::rotate(first, middle, last);
                return;
            }

            // Cap the merge buffer at L2 size (keeps extra space O(log n)) by splitting and rotating larger overlaps.
            if (std::size_t(std::min(left_len, right_len)) > Cache<T>::l2_bytes / sizeof(T)) {
                T *a, *b;
                if (left_len >= right_len) {
                    a = first + left_len / 2;
                    b = std::lower_bound(middle, last, *a);
                } else {
                    b = middle + right_len / 2;
                    a = std::upper_bound(first, middle, *b);
                }
                T *cut = std::rotate(a, middle, b);
                merge_runs(first, a, cut);
                merge_runs(cut, b, last);
                return;
            }

            // Skewed runs predict well with branches; balanced runs use a branchless merge loop.
            auto buffer = std::make_unique_for_overwrite<T[]>(std::min(left_len, right_len));
            T *tmp = buffer.get();
            if (left_len <= right_len) {
                std::copy(first, middle, tmp);
                T *a = tmp, *b = middle, *dest = first;
                if (left_len < right_len / 8) {
                    while (a != tmp + left_len && b != last) {
                        if (*a <= *b) *dest++ = *a++;
                        else *dest++ = *b++;
                    }
                }
                while (a != tmp + left_len && b != last) {
                    const T x = *a, y = *b;
                    const bool take_left = x <= y;
                    *dest++ = take_left ? x : y;
                    a += take_left;
                    b += !take_left;
                }
                std::copy(a, tmp + left_len, dest);
            } else {
                std::copy(middle, last, tmp);
                T *a = middle, *b = tmp + right_len, *dest = last;
                if (right_len < left_len / 8) {
                    while (a != first && b != tmp) {
                        if (a[-1] > b[-1]) *--dest = *--a;
                        else *--dest = *--b;
                    }
                }
                while (a != first && b != tmp) {
                    const T x = a[-1], y = b[-1];
                    const bool take_left = x > y;
                    *--dest = take_left ? x : y;
                    a -= take_left;
                    b -= !take_left;
                }
                std::copy(tmp, b, first);
            }
        }

        // Reject unsorted input in 8 keys, then scan 64-key blocks branchlessly.
        template<bool descending, typename T>
        static T *run_end(T *first, T *last) {
            auto breaks_run = [](T a, T b) {
                if constexpr (descending) return a < b;
                else return b < a;
            };

            T *p = first + 1;
            if (last - p >= 8) {
                for (int i = 0; i < 8; ++i, ++p) {
                    if (breaks_run(p[-1], p[0])) return p;
                }
            }
            while (last - p >= 64) {
                unsigned broken = 0;
                for (int i = 0; i < 64; ++i) {
                    broken |= breaks_run(p[i - 1], p[i]);
                }
                if (broken) break;
                p += 64;
            }
            while (p != last && !breaks_run(p[-1], p[0])) ++p;
            return p;
        }

        // Four independent min/max lanes let the CPU overlap comparisons.
        template<typename T>
        static std::pair<T, T> extrema(const T *data, std::size_t n) {
            std::array<T, 4> low, high;
            low.fill(data[0]);
            high.fill(data[0]);

            std::size_t i = 0;
            for (; i + 4 <= n; i += 4) {
                for (unsigned j = 0; j < 4; ++j) {
                    low[j] = std::min(low[j], data[i + j]);
                    high[j] = std::max(high[j], data[i + j]);
                }
            }
            for (; i < n; ++i) {
                low[0] = std::min(low[0], data[i]);
                high[0] = std::max(high[0], data[i]);
            }
            return {*std::min_element(low.begin(), low.end()), *std::max_element(high.begin(), high.end())};
        }

        template<typename T>
        static bool homogeneous(const T *data, std::size_t n) {
            return data[0] == data[n - 1] && data[0] == data[n / 2] &&
                   std::all_of(data + 1, data + n, [&](T x) { return x == data[0]; });
        }

        template<typename T>
        static void insertion_sort(T *first, T *last) {
            for (T *p = first + 1; p < last; ++p) {
                const T x = *p;
                if (x < p[-1]) {
                    T *q = p;
                    do {
                        *q = q[-1];
                        --q;
                    } while (q != first && x < q[-1]);
                    *q = x;
                }
            }
        }

        template<typename T>
        static void swap_fragment(T *a, T *b, int cap, T *tmp) {
            std::memcpy(tmp, a, cap * sizeof(T));
            std::memcpy(a, b, cap * sizeof(T));
            std::memcpy(b, tmp, cap * sizeof(T));
        }
    }
}
