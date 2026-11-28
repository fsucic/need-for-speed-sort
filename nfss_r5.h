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
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <type_traits>
#include <utility>

#ifndef NFS_R5_CACHE_LINE_BYTES
#define NFS_R5_CACHE_LINE_BYTES 64
#endif
#ifndef NFS_R5_L1D_BYTES
#define NFS_R5_L1D_BYTES (32 * 1024)
#endif
#ifndef NFS_R5_L2_BYTES
#define NFS_R5_L2_BYTES (1024 * 1024)
#endif

namespace nfs_r5 {
    /*
     * Value-interpolation NFS Sort, round 5 (32/64-bit integers and IEEE floats;
     * unstable; NaNs placed last with zero signs and NaN payloads preserved).
     *
     * Usage: nfs_r5::sort(v.begin(), v.end());
     * Memory: one fixed heap buffer of at most 896 KiB per call plus O(log n) stack.
     * Tuning: define NFS_R5_CACHE_LINE_BYTES, NFS_R5_L1D_BYTES and NFS_R5_L2_BYTES
     *         (defaults 64 B / 32 KiB / 1 MiB) identically across translation units
     *         to match the CPU's fast on-chip memory.
     *
     * Main stages:
     * - Prechecks: merge up to 4 long sorted runs, count inputs with few distinct
     *   values in a hash table, or pull out and merge back displaced keys when the
     *   input is nearly sorted.
     * - Distribution: peel extreme outliers at the root, then scatter keys into
     *   proportional value buckets (using unused bits of each word for narrow
     *   integer ranges, or in-place block shuffling otherwise).
     * - Small buckets (leaves): direct counting for dense integers, 16-bit offset
     *   pages sorted with vector networks for medium integer spans, or a ~4x slot
     *   table with an occupancy bitmap for well-spread keys.
     */
    namespace internal {
        // Buffer limits derived from the CPU's on-chip memory sizes, capped at 896 KiB.
        constexpr std::size_t line_bytes = NFS_R5_CACHE_LINE_BYTES;
        constexpr std::size_t l1_bytes = NFS_R5_L1D_BYTES;
        constexpr std::size_t l2_bytes = NFS_R5_L2_BYTES;
        static_assert(std::has_single_bit(line_bytes) && line_bytes >= 8);
        static_assert(l1_bytes >= line_bytes && l2_bytes >= l1_bytes);
        constexpr std::size_t buffer_limit = 896 * 1024;
        constexpr std::size_t merge_bytes = std::min(buffer_limit, l2_bytes);
        // Find the end of a sorted run; exits in 8 keys on random input, then checks 64 keys at a time.
        template <bool descending, typename T> static T *run_end(T *first, T *last) {
            auto breaks_run = [](T a, T b) {
                if constexpr (descending)
                    return a < b;
                else
                    return b < a;
            };
            T *p = first + 1;
            if (last - p >= 8) {
                for (int i = 0; i < 8; ++i, ++p)
                    if (breaks_run(p[-1], p[0])) return p;
            }
            while (last - p >= 64) {
                unsigned broken = 0;
                for (int i = 0; i < 64; ++i) broken |= breaks_run(p[i - 1], p[i]);
                if (broken) break;
                p += 64;
            }
            while (p != last && !breaks_run(p[-1], p[0])) ++p;
            return p;
        }

        // Merge two runs of similar length 8 keys at a time with fixed compare-swap steps instead of per-key branches (speeds up organ-pipe and multi-run inputs).
        template <unsigned W, typename T, typename It, typename Less>
        static void merge_blocks(It a, It a_end, It b, It b_end, It out, Less less) {
            std::array<T, W> carry;
            std::copy_n(a, W, carry.begin());
            a += W;
            while (a_end - a >= W && b_end - b >= W) {
                const bool take_b = less(*b, *a);
                It incoming = take_b ? b : a;
                if (take_b)
                    b += W;
                else
                    a += W;
                std::array<T, W> lower;
                for (unsigned i = 0; i < W; ++i) {
                    const T x = carry[i], y = incoming[W - 1 - i];
                    const bool down = less(y, x);
                    lower[i] = down ? y : x;
                    carry[i] = down ? x : y;
                }
                for (unsigned stride = W / 2; stride; stride /= 2)
                    for (unsigned group = 0; group < W; group += 2 * stride)
                        for (unsigned j = group; j < group + stride; ++j) {
                            auto exchange = [&](auto &v) {
                                const T x = v[j], y = v[j + stride];
                                const bool down = less(y, x);
                                v[j] = down ? y : x;
                                v[j + stride] = down ? x : y;
                            };
                            exchange(lower);
                            exchange(carry);
                        }
                std::copy(lower.begin(), lower.end(), out);
                out += W;
            }
            if (a_end - a >= W) {
                std::swap(a, b);
                std::swap(a_end, b_end);
            }
            std::array<T, 2 * W> tail;
            auto end = std::merge(a, a_end, carry.begin(), carry.end(), tail.begin(), less);
            auto p = tail.begin();
            while (p != end && b != b_end) {
                if (less(*b, *p))
                    *out++ = *b++;
                else
                    *out++ = *p++;
            }
            out = std::copy(p, end, out);
            if (out != b) std::copy(b, b_end, out);
        }

        // Merge two adjacent sorted runs in place using at most the shared 896 KiB scratch buffer.
        template <typename T> static void merge_runs(T *first, T *middle, T *last, std::unique_ptr<T[]> &buffer) {
            if (middle == first || middle == last || middle[-1] <= *middle) return;

            first = std::upper_bound(first, middle, *middle);
            last = std::lower_bound(middle, last, middle[-1]);
            const auto left_len = middle - first;
            const auto right_len = last - middle;

            if (left_len == 1 || right_len == 1) {
                std::rotate(first, middle, last);
                return;
            }

            // Split the larger run until one trimmed side fits the shared buffer.
            if (std::size_t(std::min(left_len, right_len)) > merge_bytes / sizeof(T)) {
                T *a, *b;
                if (left_len >= right_len) {
                    a = first + left_len / 2;
                    b = std::lower_bound(middle, last, *a);
                } else {
                    b = middle + right_len / 2;
                    a = std::upper_bound(first, middle, *b);
                }
                T *cut = std::rotate(a, middle, b);
                merge_runs(first, a, cut, buffer);
                merge_runs(cut, b, last, buffer);
                return;
            }

            if (!buffer) buffer = std::make_unique_for_overwrite<T[]>(merge_bytes / sizeof(T));
            T *tmp = buffer.get();
            // Balanced runs merge 8 keys at a time; lopsided runs use a simple loop since the longer side almost always wins.
            if (left_len <= right_len) {
                std::copy(first, middle, tmp);
                if (left_len >= 32 && left_len >= right_len / 8) {
                    merge_blocks<8, T>(tmp, tmp + left_len, middle, last, first, [](T a, T b) { return a < b; });
                    return;
                }
                T *a = tmp, *b = middle, *dest = first;
                while (a != tmp + left_len && b != last) {
                    if (*a <= *b)
                        *dest++ = *a++;
                    else
                        *dest++ = *b++;
                }
                std::copy(a, tmp + left_len, dest);
            } else {
                std::copy(middle, last, tmp);
                if (right_len >= 32 && right_len >= left_len / 8) {
                    auto rev = [](T *p) { return std::make_reverse_iterator(p); };
                    merge_blocks<8, T>(rev(tmp + right_len), rev(tmp), rev(middle), rev(first), rev(last),
                                       [](T a, T b) { return a > b; });
                    return;
                }
                T *a = middle, *b = tmp + right_len, *dest = last;
                while (a != first && b != tmp) {
                    if (a[-1] > b[-1])
                        *--dest = *--a;
                    else
                        *--dest = *--b;
                }
                std::copy(tmp, b, first);
            }
        }

        // Fast path for sorted, reversed, organ-pipe, and half-sorted inputs: detect and merge up to 4 long runs.
        template <typename T, typename Sort> static bool sort_runs(T *first, T *last, Sort sort_remainder) {
            std::unique_ptr<T[]> buffer;
            const auto n = last - first;
            T *bounds[5]{first};
            bool descending[4]{};
            T *p = first;
            unsigned runs = 0;
            while (p != last && runs < 4) {
                bool down = p + 1 != last && p[1] < p[0];
                T *end = down ? run_end<true>(p, last) : run_end<false>(p, last);
                if (end != last && end - p < std::max<std::ptrdiff_t>(16, n / 16)) break;
                descending[runs] = down;
                bounds[++runs] = end;
                p = end;
            }
            if (p != last) {
                if (runs && p - first >= n / 2 && runs < 4) {
                    sort_remainder(p, last);
                    bounds[++runs] = last;
                } else if (!runs && n >= 32) {
                    const bool down = last[-1] < last[-2];
                    T *start = last - 2;
                    if (down)
                        while (start != first && start[-1] >= *start) --start;
                    else
                        while (start != first && start[-1] <= *start) --start;
                    if (last - start < n / 2) return false;
                    sort_remainder(first, start);
                    if (down) std::reverse(start, last);
                    merge_runs(first, start, last, buffer);
                    return true;
                } else
                    return false;
            }
            for (unsigned i = 0; i < runs; ++i)
                if (descending[i]) std::reverse(bounds[i], bounds[i + 1]);
            for (unsigned width = 1; width < runs; width *= 2) {
                for (unsigned i = 0; i + width < runs; i += 2 * width)
                    merge_runs(bounds[i], bounds[i + width], bounds[std::min(runs, i + 2 * width)], buffer);
            }
            return true;
        }

        // Read a key's raw bits as an unsigned integer so floats hash and compare by exact bit pattern.
        template <typename T> static auto key_bits(T value) {
            using U = std::conditional_t<sizeof(T) == 8, std::uint64_t, std::uint32_t>;
            return std::bit_cast<U>(value);
        }

        // Sample keys across the array to estimate whether most values are duplicates.
        template <unsigned Samples, typename T, typename Hash>
        static unsigned sample_repeats(const T *data, std::size_t n, Hash hash) {
            std::array<T, 2 * Samples> keys;
            std::array<bool, 2 * Samples> occupied{};
            const std::size_t stride = n / Samples;
            constexpr unsigned shift = 64 - std::bit_width(2 * Samples - 1);
            std::uint32_t random = 42;
            unsigned repeats = 0;
            for (unsigned i = 0; i < Samples; ++i) {
                random = random * 1664525u + 1013904223u;
                const auto j = i * stride + ((std::uint64_t(random) * stride) >> 32);
                const T x = data[j];
                const auto slot = hash(x) >> shift;
                repeats += occupied[slot] && key_bits(x) == key_bits(keys[slot]);
                keys[slot] = x;
                occupied[slot] = true;
            }
            return repeats;
        }

        // Fast path for inputs with few distinct values: count keys in a hash table, sort the unique keys, and expand counts.
        template <typename T> static bool count_repeated(T *data, std::size_t n) {
            if (n < 1024) return false;
            auto hash = [](T x) {
                std::uint64_t h = key_bits(x);
                return (h ^ (h >> 33)) * UINT64_C(11400714819323198485);
            };
            const unsigned sample_size = n < 32768 ? 128 : n < 131072 ? 256 : 512;
            const unsigned repeats = sample_size == 128   ? sample_repeats<128>(data, n, hash)
                                     : sample_size == 256 ? sample_repeats<256>(data, n, hash)
                                                          : sample_repeats<512>(data, n, hash);
            if (repeats < 4 || n < std::uint64_t(sample_size) * sample_size * 2 / repeats) return false;

            constexpr unsigned max_bits = 15;
            constexpr unsigned capacity = 1u << max_bits;
            struct Table {
                T keys[capacity];
                std::size_t counts[capacity];
                unsigned occupied_slots[capacity / 2];
            };
            static_assert(sizeof(Table) <= buffer_limit);
            auto table = std::make_unique_for_overwrite<Table>();
            const unsigned bits =
                std::clamp<unsigned>(std::bit_width(8u * sample_size * sample_size / repeats), 8, max_bits);
            const unsigned bins = 1u << bits, mask = bins - 1;
            std::fill_n(table->keys, bins, data[0]);
            std::fill_n(table->counts, bins, 0);
            const unsigned first = unsigned(hash(data[0]) >> (64 - bits));
            table->occupied_slots[0] = first;
            unsigned distinct = 1;
            for (std::size_t i = 0; i < n; ++i) {
                const T x = data[i];
                unsigned h = unsigned(hash(x) >> (64 - bits));
                if (key_bits(table->keys[h]) != key_bits(x)) {
                    unsigned probes = 0;
                    while (table->counts[h] && key_bits(table->keys[h]) != key_bits(x)) {
                        if (++probes == 32) return false;
                        h = (h + 1) & mask;
                    }
                    if (!table->counts[h]) {
                        if (distinct == bins / 2) return false;
                        table->occupied_slots[distinct++] = h;
                        table->keys[h] = x;
                    }
                }
                ++table->counts[h];
            }
            auto used = table->occupied_slots;
            std::sort(used, used + distinct, [&](unsigned a, unsigned b) { return table->keys[a] < table->keys[b]; });
            for (unsigned i = 0; i < distinct; ++i) {
                const unsigned h = used[i];
                data = std::fill_n(data, table->counts[h], table->keys[h]);
            }
            return true;
        }
        // Plain insertion sort for tiny slices (< 32 keys) and local cleanup.
        template <typename T> static void insertion_sort(T *first, T *last) {
            for (T *p = first + 1; p < last; ++p) {
                const T value = *p;
                T *q = p;
                while (q != first && value < q[-1]) {
                    *q = q[-1];
                    --q;
                }
                *q = value;
            }
        }

        // Shared scratch layout (<= 896 KiB): overlays general scatter/sparse buffers with 16-bit offset-page storage.
        constexpr unsigned slots = 1u << 16;
        constexpr unsigned max_bins =
            std::clamp<std::size_t>(std::bit_floor(l1_bytes / (2 * sizeof(std::size_t))), 32, 2048);
        template <typename T> constexpr unsigned block_size = std::min<std::size_t>(32, 4 * (line_bytes / sizeof(T)));
        template <typename T> struct Workspace {
            struct General {
                T items[slots + 64];
                T overflow[slots / 32];
                std::uint64_t bitmap[slots / 64 + 1];
            };
            struct Pages {
                std::uint16_t values[2 * slots + 88];
                unsigned cursor[slots];
            };
            union {
                General general;
                Pages pages;
            };
            Workspace() { ::new (&general) General; }
            std::uint16_t counts[slots];
            unsigned used[max_bins];
            std::size_t next[max_bins];
            std::size_t ends[5][max_bins];
        };
        template <typename T>
        constexpr unsigned leaf_size = std::clamp<std::size_t>(l2_bytes / (4 * sizeof(T)), 256, slots - 1);
        template <typename T>
        constexpr unsigned target_leaf = std::clamp<std::size_t>(l1_bytes / (2 * sizeof(T)), 256, slots / 4);
        // In-place bucket scatter: batch keys into small blocks, cycle full blocks into place, then append leftover tails.
        template <typename T, typename Classify>
        static void fragment_scatter(T *data, std::size_t n, unsigned bins, Classify classify, Workspace<T> &ws,
                                     std::size_t *ends) {
            constexpr unsigned block_size = internal::block_size<T>;
            static_assert(max_bins * block_size <= slots);
            auto *used = ws.used;
            auto *buffers = ws.general.items;
            auto *next = ws.next;
            std::fill_n(used, bins, 0u);
            std::fill_n(ends, bins, std::size_t(0));
            std::size_t written = 0;
            // Pass 1: collect keys into per-bucket blocks and flush full blocks sequentially into data.
            for (std::size_t i = 0; i < n; ++i) {
                const T x = data[i];
                const unsigned b = classify(x);
                const unsigned slot = used[b]++;
                T *buf = buffers + b * block_size;
                buf[slot] = x;
                if (slot + 1 == block_size) {
                    std::memcpy(data + written, buf, block_size * sizeof(T));
                    written += block_size;
                    ends[b] += block_size;
                    used[b] = 0;
                }
            }
            std::size_t packed = 0;
            for (unsigned b = 0; b < bins; ++b) {
                next[b] = packed;
                packed += ends[b];
                ends[b] = packed;
            }
            // Pass 2: cycle full blocks into their target buckets in place.
            for (unsigned b = 0; b < bins; ++b) {
                while (next[b] != ends[b]) {
                    T *block = data + next[b];
                    unsigned owner = classify(*block);
                    if (owner != b) {
                        T carried[block_size];
                        std::memcpy(carried, block, sizeof(carried));
                        do {
                            T *dest = data + next[owner];
                            next[owner] += block_size;
                            std::swap_ranges(carried, carried + block_size, dest);
                            owner = classify(carried[0]);
                        } while (owner != b);
                        std::memcpy(block, carried, sizeof(carried));
                    }
                    next[b] += block_size;
                }
            }
            // Pass 3: shift full blocks right to make room and copy each bucket's partial tail into place.
            std::size_t end = n;
            for (unsigned b = bins; b-- > 0;) {
                const std::size_t start = b ? ends[b - 1] : 0;
                const std::size_t length = ends[b] - start;
                const std::size_t tail = end - used[b];
                const std::size_t move = std::min(length, tail - ends[b]);
                std::memcpy(data + tail - move, data + start, move * sizeof(T));
                std::memcpy(data + tail, buffers + b * block_size, used[b] * sizeof(T));
                ends[b] = end;
                end = tail - length;
            }
        }

        // One-pass leaf sort for well-spread keys: place into a ~4x slot table (1 bit per slot tracks occupancy), sweep in order, and fix rare collisions.
        template <typename T, typename Classify, typename Read>
        static bool sparse_read(T *data, std::size_t n, Classify classify, unsigned bins, Workspace<T> &ws, Read read) {
            auto *taken = ws.general.bitmap;
            const unsigned words = (bins + 63) / 64 + 1;
            std::fill_n(taken, words, UINT64_C(0));
            T *items = ws.general.items;
            auto *overflow = ws.general.overflow;
            unsigned spilled = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const T value = read(i);
                const unsigned bin = classify(value);
                unsigned word = bin >> 6;
                auto free = ~taken[word] & (~UINT64_C(0) << (bin & 63));
                if (!free) {
                    free = ~taken[++word];
                    if (!free) {
                        if (spilled >= n / 32) return false;
                        overflow[spilled++] = value;
                        continue;
                    }
                }
                const unsigned bit = std::countr_zero(free);
                taken[word] |= UINT64_C(1) << bit;
                items[word * 64 + bit] = value;
            }
            // Sweep occupied slots in order, carrying the running max to resolve adjacent swaps before insertion.
            T *out = data;
            T carry{};
            bool started = false;
            for (unsigned w = 0; w < words; ++w) {
                auto present = taken[w];
                while (present) {
                    T x = items[w * 64 + std::countr_zero(present)];
                    present &= present - 1;
                    if (!started) {
                        carry = x;
                        started = true;
                        continue;
                    }
                    const T previous = carry;
                    const bool below = x < previous;
                    carry = below ? previous : x;
                    x = below ? x : previous;
                    T *at = out++;
                    while (at != data && x < at[-1]) {
                        *at = at[-1];
                        --at;
                    }
                    *at = x;
                }
            }
            *out++ = carry;
            if (spilled) {
                std::sort(overflow, overflow + spilled);
                auto right = overflow + spilled;
                T *dest = data + n;
                while (out != data && right != overflow) {
                    if (out[-1] > right[-1])
                        *--dest = *--out;
                    else
                        *--dest = *--right;
                }
                std::copy(overflow, right, data);
            }
            return true;
        }

        template <typename T, typename Classify>
        static bool sparse_leaf(T *data, std::size_t n, Classify classify, unsigned bins, Workspace<T> &ws) {
            return sparse_read(data, n, classify, bins, ws, [&](std::size_t i) { return data[i]; });
        }

        // Cheap 64-point check to spot nearly sorted inputs (such as a small share of random swaps).
        template <typename T> static bool looks_ordered(const T *data, std::size_t n) {
            if (n < 128) return false;
            unsigned adjacent = 0, distant = 0;
            for (unsigned i = 0; i < 64; ++i) {
                const std::size_t j = i * ((n - 2) / 64);
                adjacent += data[j + 1] < data[j];
                if (i) distant += data[j] < data[j - (n - 2) / 64];
            }
            return adjacent <= 8 && distant <= 10;
        }

        // Exact min/max scan across four independent trackers so the CPU can run comparisons in parallel.
        template <typename T> static std::pair<T, T> extrema(const T *data, std::size_t n) {
            std::array<T, 4> low, high;
            low.fill(data[0]);
            high.fill(data[0]);
            std::size_t i = 0;
            for (; i + 4 <= n; i += 4)
                for (unsigned j = 0; j < 4; ++j) {
                    low[j] = std::min(low[j], data[i + j]);
                    high[j] = std::max(high[j], data[i + j]);
                }
            for (; i < n; ++i) {
                low[0] = std::min(low[0], data[i]);
                high[0] = std::max(high[0], data[i]);
            }
            return {*std::min_element(low.begin(), low.end()), *std::max_element(high.begin(), high.end())};
        }

        // Fast path for nearly sorted inputs: pull out displaced keys (up to n/6), sort only those, and merge them back.
        template <bool Batched, typename T, typename Sort>
        static bool repair_order(T *data, std::size_t n, Workspace<T> &ws, Sort sort_extra) {
            auto *extra = ws.general.items;
            std::size_t count = 0;
            const std::size_t budget = std::min<std::size_t>(slots, n / 6);
            T *kept = data;
            T *p = data;
            while (p != data + n) {
                const auto block = Batched ? std::min<std::ptrdiff_t>(8, data + n - p) : data + n - p;
                if constexpr (Batched) {
                    if (block == 8 && (kept == data || *p >= kept[-1])) {
                        unsigned broken = 0;
                        for (unsigned j = 1; j < 8; ++j) broken |= p[j] < p[j - 1];
                        if (!broken) {
                            if (kept != p) std::memmove(kept, p, 8 * sizeof(T));
                            kept += 8;
                            p += 8;
                            continue;
                        }
                    }
                }
                T *end = p + block;
                for (; p != end; ++p) {
                    const T x = *p;
                    if (kept == data || x >= kept[-1]) {
                        *kept++ = x;
                        continue;
                    }
                    if (count + 2 > budget) {
                        std::copy(extra, extra + count, kept);
                        return false;
                    }
                    if (p + 1 == data + n || p[1] < kept[-1]) {
                        extra[count++] = *--kept;
                        if (kept == data || x >= kept[-1]) {
                            *kept++ = x;
                            continue;
                        }
                    }
                    extra[count++] = x;
                }
            }
            if (!count) return true;
            if (count < 128)
                std::sort(extra, extra + count);
            else {
                std::copy_n(extra, count, kept);
                sort_extra(kept, count);
                extra = ws.general.items;
                std::copy_n(kept, count, extra);
            }
            // Merge the sorted displaced keys back into the ordered backbone (8 keys at a time when breaks are rare).
            auto right = extra + count;
            T *dest = data + n;
            if constexpr (Batched) {
                while (right != extra && kept != data) {
                    const T x = *--right;
                    while (kept - data >= 8 && kept[-8] > x) {
                        kept -= 8;
                        dest -= 8;
                        std::memmove(dest, kept, 8 * sizeof(T));
                    }
                    while (kept != data && kept[-1] > x) *--dest = *--kept;
                    *--dest = x;
                }
            } else {
                while (kept != data && right != extra) {
                    if (kept[-1] > right[-1])
                        *--dest = *--kept;
                    else
                        *--dest = *--right;
                }
            }
            std::copy(extra, right, data);
            return true;
        }

        // Value-interpolation map: assigns keys to proportional buckets using exact integer math (128-bit multiply-and-shift instead of division) and recovers exact bucket bounds.
        template <typename T> struct ValueMap {
            T low;
            std::uint64_t width = 0;
            std::uint64_t multiplier = 0;
            double scale = 0;

            ValueMap(T lo, T hi, unsigned bins) : low(lo) {
                if constexpr (std::is_integral_v<T>) {
                    const auto span = distance(hi);
                    width = span / bins + 1;
#if defined(__SIZEOF_INT128__)
                    if (width > 1) multiplier = std::uint64_t((__uint128_t(bins) << 64) / (__uint128_t(span) + 1));
#endif
                } else
                    scale = (bins - 1) / (double(hi) - double(lo));
            }

            std::uint64_t distance(T x) const
                requires std::is_integral_v<T>
            {
                using U = std::make_unsigned_t<T>;
                return U(U(x) - U(low));
            }

            std::pair<T, T> bounds(unsigned b, T high) const
                requires std::is_integral_v<T>
            {
                using U = std::make_unsigned_t<T>;
                const auto value = [&](std::uint64_t d) { return T(U(U(low) + U(d))); };
#if defined(__SIZEOF_INT128__)
                if (width > 1) {
                    auto cut = [&](unsigned i) { return ((__uint128_t(i) << 64) + multiplier - 1) / multiplier; };
                    return {value(std::uint64_t(cut(b))),
                            value(std::uint64_t(std::min<__uint128_t>(distance(high), cut(b + 1) - 1)))};
                }
#endif
                const auto start = b * width;
                return {value(start), value(start + std::min(width - 1, distance(high) - start))};
            }

            unsigned operator()(T x) const {
                if constexpr (std::is_integral_v<T>) {
                    const auto d = distance(x);
                    if (width == 1) return unsigned(d);
#if defined(__SIZEOF_INT128__)
                    return unsigned((__uint128_t(d) * multiplier) >> 64);
#else
                    return unsigned(d / width);
#endif
                } else
                    return unsigned((double(x) - double(low)) * scale);
            }
        };

        // Fallback leaf when sparse placement overflows: bucket into scratch, sort any bucket over 24 keys, and finish with insertion sort.
        template <typename T, typename Position>
        static void histogram_leaf(T *data, unsigned n, unsigned bins, Position position, Workspace<T> &ws) {
            auto *ends = ws.counts;
            std::fill_n(ends, bins, 0);
            for (unsigned i = 0; i < n; ++i) ++ends[position(data[i])];
            std::exclusive_scan(ends, ends + bins, ends, 0u);
            for (unsigned i = 0; i < n; ++i) ws.general.items[ends[position(data[i])]++] = data[i];
            unsigned start = 0;
            for (unsigned b = 0; b < bins; ++b) {
                const unsigned end = ends[b];
                if (end - start > 24) std::sort(ws.general.items + start, ws.general.items + end);
                start = end;
            }
            insertion_sort(ws.general.items, ws.general.items + n);
            std::copy_n(ws.general.items, n, data);
        }

// Fixed compare-swap sorting networks for 16-bit offsets (8 values per vector instruction, up to 256 values) so the CPU never has to guess branches.
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 12)
        using Lanes = std::uint16_t __attribute__((vector_size(16)));

        template <unsigned Stride, unsigned Group, std::size_t... I>
        static inline Lanes exchange_lanes(Lanes values, std::index_sequence<I...>) {
            const auto other = __builtin_shufflevector(values, values, (I ^ Stride)...);
            const auto low = (values < other) ? values : other;
            const auto high = (values > other) ? values : other;
            constexpr Lanes mask{std::uint16_t(0u - unsigned(bool(I & Group) == bool(I & Stride)))...};
            return (low & mask) | (high & ~mask);
        }

        template <unsigned Stride, unsigned Group> static inline Lanes exchange(Lanes v) {
            return exchange_lanes<Stride, Group>(v, std::make_index_sequence<8>{});
        }

        static inline Lanes reverse_lanes(Lanes v) { return __builtin_shufflevector(v, v, 7, 6, 5, 4, 3, 2, 1, 0); }

        template <unsigned Count> static inline void merge_values(Lanes *blocks) {
            if constexpr (Count == 1) {
                blocks[0] = exchange<1, 8>(exchange<2, 8>(exchange<4, 8>(blocks[0])));
            } else {
                for (unsigned i = 0; i < Count / 2; ++i) {
                    const auto a = blocks[i], b = blocks[i + Count / 2];
                    blocks[i] = (a < b) ? a : b;
                    blocks[i + Count / 2] = (a > b) ? a : b;
                }
                merge_values<Count / 2>(blocks);
                merge_values<Count / 2>(blocks + Count / 2);
            }
        }

        template <unsigned Count> static inline void sort_values(Lanes *blocks) {
            if constexpr (Count == 1) {
                blocks[0] = exchange<1, 4>(exchange<2, 4>(exchange<1, 2>(blocks[0])));
            } else {
                sort_values<Count / 2>(blocks);
                sort_values<Count / 2>(blocks + Count / 2);
                if constexpr (Count == 2) {
                    blocks[1] = reverse_lanes(blocks[1]);
                } else {
                    for (unsigned i = 0; i < Count / 4; ++i) {
                        auto &a = blocks[Count / 2 + i], &b = blocks[Count - 1 - i];
                        const auto left = reverse_lanes(a), right = reverse_lanes(b);
                        a = right;
                        b = left;
                    }
                }
            }
            merge_values<Count>(blocks);
        }

        template <unsigned Count> static inline void sort_block(std::uint16_t *data) {
            Lanes blocks[Count];
            std::memcpy(blocks, data, sizeof(blocks));
            sort_values<Count>(blocks);
            std::memcpy(data, blocks, sizeof(blocks));
        }
#else
        template <unsigned Count> static inline void sort_block(std::uint16_t *data) {
            std::sort(data, data + 8 * Count);
        }
#endif

        // Check whether an integer leaf fits in 16-bit offset pages (speeds up dense ID datasets like Facebook).
        static bool uses_offset_pages(std::size_t n, std::uint64_t span) {
            return n >= 128 && n < slots && span >= 256 * n && span < (1u << 30) && 1 + span / UINT16_MAX <= n / 2;
        }

        // Split a medium-span integer leaf into slices <= 65,535 wide, sort their 16-bit offsets with vector networks grouped by size, and rebuild full keys.
        template <typename T>
        static bool network_leaf(T *data, unsigned n, T low, std::uint64_t span, Workspace<T> &ws) {
            using U = std::make_unsigned_t<T>;
            if (!uses_offset_pages(n, span)) return false;
            const unsigned requested = std::max(unsigned((n + 11) / 12), 1 + unsigned(span / 65535));
            const unsigned width = unsigned(span / requested) + 1;
            const unsigned bins = unsigned(span / width) + 1;
            const auto reciprocal = ((UINT64_C(1) << 48) + width - 1) / width;
            const auto bucket = [=](unsigned d) { return unsigned((d * reciprocal) >> 48); };
            auto *counts = ws.counts;
            std::fill_n(counts, bins, 0u);
            for (unsigned i = 0; i < n; ++i) ++counts[bucket(unsigned(U(U(data[i]) - U(low))))];

            // Group buckets by power-of-two size class, padded with UINT16_MAX so sorting networks run uniformly.
            auto *pages = ::new (&ws.pages) typename Workspace<T>::Pages;
            auto *values = pages->values;
            auto *cursor = pages->cursor;
            const auto size_class = [](unsigned size) { return std::min<unsigned>(std::bit_width(size - 1), 9); };
            const auto reserved = [](unsigned size, unsigned group) {
                return group < 9 ? 1u << group : (size + 7u) & ~7u;
            };
            std::array<unsigned, 10> next{};
            for (unsigned b = 0; b < bins; ++b) {
                const unsigned size = counts[b];
                if (size) {
                    const unsigned group = size_class(size);
                    next[group] += reserved(size, group);
                }
            }
            for (unsigned &length : next) length = (length + 7u) & ~7u;
            const unsigned total = std::accumulate(next.begin(), next.end(), 0u);
            std::fill_n(values, total + 8, UINT16_MAX);
            std::exclusive_scan(next.begin(), next.end(), next.begin(), 0u);
            const auto begins = next;
            for (unsigned b = 0; b < bins; ++b) {
                const unsigned size = counts[b];
                if (size) {
                    const unsigned group = size_class(size);
                    cursor[b] = next[group];
                    next[group] += reserved(size, group);
                }
            }
            for (unsigned i = 0; i < n; ++i) {
                const unsigned d = unsigned(U(U(data[i]) - U(low)));
                const unsigned b = bucket(d);
                values[cursor[b]++] = std::uint16_t(d - b * width);
            }
            const auto sort_group = [&]<unsigned Bits>() {
                for (unsigned i = begins[Bits]; i < next[Bits]; i += 1u << Bits)
                    sort_block<1u << (Bits - 3)>(values + i);
            };
            for (unsigned group = 1; group <= 2; ++group) {
                for (unsigned i = begins[group]; i < next[group]; i += 8) {
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 12)
                    Lanes v;
                    std::memcpy(&v, values + i, sizeof(v));
                    if (group == 1)
                        v = exchange<1, 8>(v);
                    else
                        v = exchange<1, 8>(exchange<2, 8>(exchange<1, 2>(v)));
                    std::memcpy(values + i, &v, sizeof(v));
#else
                    const unsigned width = 1u << group;
                    for (unsigned j = 0; j < 8; j += width) std::sort(values + i + j, values + i + j + width);
#endif
                }
            }
            sort_group.template operator()<3>();
            sort_group.template operator()<4>();
            sort_group.template operator()<5>();
            sort_group.template operator()<6>();
            sort_group.template operator()<7>();
            sort_group.template operator()<8>();

            // Rebuild full keys from base + sorted 16-bit offsets (buckets over 256 fall back to std::sort).
            T *out = data;
            for (unsigned b = 0; b < bins; ++b) {
                const unsigned count = counts[b];
                if (!count) continue;
                auto *offsets = values + cursor[b] - count;
                if (count > 256) std::sort(offsets, offsets + count);
                const U base = U(U(low) + U(b * width));
                if (data + n - out >= count + 7) {
                    for (unsigned i = 0; i < count; i += 8)
                        for (unsigned j = 0; j < 8; ++j) out[i + j] = T(U(base + offsets[i + j]));
                } else {
                    for (unsigned i = 0; i < count; ++i) out[i] = T(U(base + offsets[i]));
                }
                out += count;
            }
            ::new (&ws.general) typename Workspace<T>::General;
            return true;
        }

        // Count bucket sizes and turn them into starting write offsets.
        template <typename T, typename Map>
        static void bucket_starts(const T *data, std::size_t n, unsigned bins, Map position, std::size_t *ends) {
            std::fill_n(ends, bins, std::size_t(0));
            for (std::size_t i = 0; i < n; ++i) ++ends[position(data[i])];
            std::exclusive_scan(ends, ends + bins, ends, std::size_t(0));
        }

        // Half-word staging: when keys fit in half the element width, scatter offsets into the unused upper half of each slot in one pass.
        template <typename T> using Half = std::conditional_t<sizeof(T) == 8, std::uint32_t, std::uint16_t>;
        template <bool Upper, typename T> static Half<T> read_half(const T *slot) {
            constexpr unsigned offset = (Upper == (std::endian::native == std::endian::little)) ? sizeof(Half<T>) : 0;
            Half<T> word;
            std::memcpy(&word, reinterpret_cast<const unsigned char *>(slot) + offset, sizeof(word));
            return word;
        }
        template <typename T> static T staged_value(const T *slot, T low) {
            using U = std::make_unsigned_t<T>;
            return T(U(U(low) + read_half<true>(slot)));
        }

        template <typename T, typename Map>
        static void half_scatter(T *data, std::size_t n, unsigned bins, T low, Map map, std::size_t *ends) {
            using U = std::make_unsigned_t<T>;
            using H = Half<T>;
            constexpr unsigned offset = std::endian::native == std::endian::little ? sizeof(H) : 0;
            bucket_starts(data, n, bins, map, ends);
            for (std::size_t i = 0; i < n; ++i) {
                const H d = H(read_half<false>(data + i) - H(low));
                const unsigned b = map(T(U(U(low) + d)));
                auto *slot = reinterpret_cast<unsigned char *>(data + ends[b]++) + offset;
                std::memcpy(slot, &d, sizeof(d));
            }
        }

        // Packed-range scatter: keep source bits in the low part of each word and stash destination offsets in the free upper bits (speeds up medium integer spans).
        template <typename T> struct PackedRange {
            using U = std::make_unsigned_t<T>;
            unsigned width;
            U source_mask, stored_mask;

            PackedRange(unsigned source_bits, unsigned stored_bits)
                : width(source_bits), source_mask((U(1) << source_bits) - 1), stored_mask((U(1) << stored_bits) - 1) {}
            T read(const T *slot, T low) const {
                const U residue = U(*slot) >> width;
                return T(U(U(low) + ((residue - U(low)) & stored_mask)));
            }
            template <typename Map>
            void scatter(T *data, std::size_t n, unsigned bins, T low, Map position, std::size_t *ends) const {
                bucket_starts(data, n, bins, position, ends);

                for (std::size_t i = 0; i < n; ++i) {
                    const U raw = U(data[i]);
                    const T key = T(U(U(low) + ((raw - U(low)) & source_mask)));
                    T &dest = data[ends[position(key)]++];
                    dest = T(U((U(dest) & source_mask) | ((raw & stored_mask) << width)));
                }
            }
        };
        // Main recursive driver: handles outliers, counting leaves, offset pages, sparse leaves, and staged or block scatters.
        template <typename T>
        static void distribute(T *data, std::size_t n, Workspace<T> &ws, unsigned depth = 0, T low = T(1),
                               T high = T(0)) {
            if (n < 32) {
                if (n > 1) insertion_sort(data, data + n);
                return;
            }
            const bool inherited = low <= high;
            if (!inherited) {
                const auto bounds = extrema(data, n);
                low = bounds.first;
                high = bounds.second;
            }
            if (low == high) return;
            bool leaf = n <= leaf_size<T>;
            if constexpr (std::is_integral_v<T>) {
                using U = std::make_unsigned_t<T>;
                if (uses_offset_pages(n, U(U(high) - U(low)))) leaf = true;
            }
            if constexpr (std::is_integral_v<T>) {
                using U = std::make_unsigned_t<T>;
                const auto span = U(U(high) - U(low));
                // Dense integer range that fits in the scratch table: finish directly with counting sort (speeds up heavy-tail centers).
                if (!leaf && span < slots && span < n && n <= std::uintmax_t(std::numeric_limits<T>::max())) {
                    const unsigned values = unsigned(span) + 1;
                    std::fill_n(ws.general.items, values, T(0));
                    for (std::size_t i = 0; i < n; ++i) ++ws.general.items[U(U(data[i]) - U(low))];
                    T *out = data;
                    for (unsigned b = 0; b < values; ++b)
                        out = std::fill_n(out, ws.general.items[b], T(U(U(low) + U(b))));
                    return;
                }
            }
            unsigned bins = leaf ? std::min(slots, 4 * std::bit_ceil(unsigned(n)))
                                 : unsigned(std::min<std::size_t>(
                                       max_bins, std::max<std::size_t>(16, 1 + (n - 1) / target_leaf<T>)));
            if constexpr (std::is_integral_v<T>) {
                using U = std::make_unsigned_t<T>;
                const auto span = U(U(high) - U(low));
                // For dense integer spans, size buckets so each child fits the counting table.
                if (!leaf && span / 2 < n) {
                    const auto per_interval = std::min<std::size_t>(slots - 1, l1_bytes / sizeof(T));
                    bins = unsigned(std::clamp<std::uint64_t>(1 + span / per_interval, 2, max_bins));
                }
            }
            ValueMap<T> position(low, high, bins);
            if constexpr (std::is_floating_point_v<T>) {
                if (!(position.scale > 0 && std::isfinite(position.scale))) {
                    std::sort(data, data + n);
                    return;
                }
            }
            // Outlier peeling: sample 96 keys at the root and split off extreme tails so they don't squash the main range.
            if (depth == 0 && n >= 1024) {
                std::array<T, 96> sample;
                const auto stride = n / sample.size();
                std::uint32_t state = 73;
                for (unsigned j = 0; j < sample.size(); ++j) {
                    state = state * 1664525u + 1013904223u;
                    sample[j] = data[j * stride + ((std::uint64_t(state) * stride) >> 32)];
                }
                std::sort(sample.begin(), sample.end());
                const T a = sample[4], b = sample[sample.size() - 5];
                using Span = decltype([] {
                    if constexpr (std::is_integral_v<T>)
                        return std::make_unsigned_t<T>(0);
                    else
                        return T(0);
                }());
                const auto distance = [](T x, T y) { return Span(Span(y) - Span(x)); };
                const Span width = distance(a, b);
                if (width < distance(low, high) / 64) {
                    const Span padding = width / 8;
                    const T lo = T(Span(a) - std::min(padding, distance(low, a)));
                    const T hi = T(Span(b) + std::min(padding, distance(b, high)));
                    T *left = data, *right = data + n;
                    if (lo > low) left = std::partition(left, right, [=](T x) { return x < lo; });
                    if (hi < high) right = std::partition(left, right, [=](T x) { return x <= hi; });
                    distribute(data, left - data, ws, depth + 1);
                    distribute(left, right - left, ws, depth + 1, lo, hi);
                    distribute(right, data + n - right, ws, depth + 1);
                    return;
                }
            }
            // Leaf paths: direct counting, 16-bit offset pages, sparse table, then crowded-leaf split or histogram fallback.
            if (leaf) {
                if constexpr (std::is_integral_v<T>) {
                    const auto span = position.distance(high);
                    if (span < std::min<std::size_t>(4 * n, slots)) {
                        const auto used = unsigned(span + 1);
                        std::fill_n(ws.counts, used, 0);
                        for (unsigned i = 0; i < n; ++i) ++ws.counts[position.distance(data[i])];
                        T *out = ws.general.items;
                        for (unsigned b = 0; b < used; ++b) {
                            using U = std::make_unsigned_t<T>;
                            const T key = T(U(U(low) + U(b)));
                            const unsigned count = ws.counts[b];
                            out[0] = out[1] = key;
                            if (count > 2) std::fill_n(out + 2, count - 2, key);
                            out += count;
                        }
                        std::copy_n(ws.general.items, n, data);
                        return;
                    }
                }
                if constexpr (std::is_integral_v<T>)
                    if (network_leaf(data, unsigned(n), low, position.distance(high), ws)) return;
                if (sparse_leaf(data, n, position, bins, ws)) return;
                // Crowded leaf fallback: remeasure tight bounds and partition into up to 256 smaller buckets.
                if (n >= 256 && depth + 1 < std::size(ws.ends)) {
                    if (inherited) {
                        const auto exact = extrema(data, n);
                        if (exact.first == exact.second) return;
                        if constexpr (std::is_integral_v<T>) {
                            using U = std::make_unsigned_t<T>;
                            if (U(U(exact.second) - U(exact.first)) < position.distance(high) / 4) {
                                distribute(data, n, ws, depth);
                                return;
                            }
                        }
                        low = exact.first;
                        high = exact.second;
                    }
                    const unsigned count = std::clamp(unsigned(n) / 64, 4u, 256u);
                    ValueMap<T> local(low, high, count);
                    if constexpr (std::is_floating_point_v<T>) {
                        if (!(local.scale > 0 && std::isfinite(local.scale))) {
                            std::sort(data, data + n);
                            return;
                        }
                    }
                    std::array<unsigned, 257> ends{};
                    for (unsigned i = 0; i < n; ++i) ++ends[local(data[i]) + 1];
                    std::partial_sum(ends.begin(), ends.begin() + count + 1, ends.begin());
                    std::array<unsigned, 256> cursor;
                    std::copy_n(ends.begin(), count, cursor.begin());
                    for (unsigned i = 0; i < n; ++i) ws.general.items[cursor[local(data[i])]++] = data[i];
                    std::copy_n(ws.general.items, n, data);
                    for (unsigned b = 0; b < count; ++b) {
                        const unsigned size = ends[b + 1] - ends[b];
                        if (size > 1) distribute(data + ends[b], size, ws, depth + 1);
                    }
                    return;
                }
                if (inherited) {
                    distribute(data, n, ws, depth);
                    return;
                }
                const auto groups = std::bit_ceil(unsigned(n)) / 2;
                histogram_leaf(data, unsigned(n), groups, ValueMap<T>(low, high, groups), ws);
                return;
            }
            if (depth == std::size(ws.ends)) {
                std::sort(data, data + n);
                return;
            }
            // Choose the cheapest scatter: half-word staging, bit-packed staging, or general in-place block scatter.
            auto *ends = ws.ends[depth];
            bool half_staged = false, packed_staged = false;
            unsigned source_bits = 0, stored_bits = 0;
            if constexpr (std::is_integral_v<T>) {
                using U = std::make_unsigned_t<T>;
                const auto span = position.distance(high);
                half_staged = (std::endian::native == std::endian::little || std::endian::native == std::endian::big) &&
                              span <= std::numeric_limits<Half<T>>::max() && span / 512 < n;
                if (half_staged) {
                    half_scatter(data, n, bins, low, position, ends);
                } else {
                    source_bits = std::bit_width(span);
                    constexpr unsigned digits = std::numeric_limits<U>::digits;
                    if (source_bits < digits && 2 * source_bits <= digits + std::bit_width(bins - 1) + 1) {
                        const auto [lo, hi] = position.bounds(0, high);
                        stored_bits = std::bit_width(U(U(hi) - U(lo)));
                        packed_staged = source_bits + stored_bits <= digits;
                        if (packed_staged)
                            PackedRange<T>(source_bits, stored_bits).scatter(data, n, bins, low, position, ends);
                    }
                }
            }
            if (!half_staged && !packed_staged) fragment_scatter(data, n, bins, position, ws, ends);
            // Unpack staged children (feeding eligible half-staged leaves straight into sparse_read) and recurse.
            std::size_t first = 0;
            for (unsigned b = 0; b < bins; ++b) {
                const auto child = ends[b] - first;
                if (!child) continue;
                T *start = data + first;
                if constexpr (std::is_integral_v<T>) {
                    const bool inherit = child > 1 && child <= n / 4;
                    const auto [lo, hi] =
                        packed_staged || inherit ? position.bounds(b, high) : std::pair<T, T>{T(1), T(0)};
                    if (packed_staged) {
                        const PackedRange<T> packed(source_bits, stored_bits);
                        for (std::size_t j = 0; j < child; ++j) start[j] = packed.read(start + j, lo);
                    } else if (half_staged) {
                        if (inherit && child >= 32 && child <= leaf_size<T>) {
                            const unsigned local_bins = std::min(slots, 4 * std::bit_ceil(unsigned(child)));
                            ValueMap<T> local(lo, hi, local_bins);
                            const auto span = local.distance(hi);
                            if (!uses_offset_pages(child, span) && span >= std::min<std::size_t>(4 * child, slots) &&
                                sparse_read(start, child, local, local_bins, ws,
                                            [&](std::size_t j) { return staged_value(start + j, low); })) {
                                first = ends[b];
                                continue;
                            }
                        }
                        for (std::size_t j = 0; j < child; ++j) start[j] = staged_value(start + j, low);
                    }
                    if (inherit)
                        distribute(start, child, ws, depth + 1, lo, hi);
                    else if (child > 1)
                        distribute(start, child, ws, depth + 1);
                } else if (child > 1)
                    distribute(start, child, ws, depth + 1);
                first = ends[b];
            }
        }

        // Split a large nearly-sorted input with median-of-3 pivots until each chunk fits repair_order's scratch budget.
        template <bool Batched, typename T>
        static void split_ordered(T *data, std::size_t n, Workspace<T> &ws, std::size_t target) {
            if (n <= target) {
                auto sort_extra = [&](T *extra, std::size_t count) { distribute(extra, count, ws); };
                if (!repair_order<Batched>(data, n, ws, sort_extra)) distribute(data, n, ws);
                return;
            }
            std::array<T, 3> sample{data[n / 4], data[n / 2], data[n - n / 4]};
            std::sort(sample.begin(), sample.end());
            const T pivot = sample[1];
            T *middle = std::partition(data, data + n, [=](T x) { return x < pivot; });
            const std::size_t left = middle - data, right = n - left;
            if (std::min(left, right) < n / 8) {
                distribute(data, left, ws);
                distribute(middle, right, ws);
                return;
            }
            split_ordered<Batched>(data, left, ws, target);
            split_ordered<Batched>(middle, right, ws, target);
        }
    }

    /**
     * Sort a writable contiguous range of 32/64-bit integers or IEEE floats by value interpolation.
     * NaNs follow all numbers. The signs of zeros and the original NaN payloads are preserved.
     * Unstable. Uses at most 896 KiB of heap scratch per call and O(log n) stack space.
     * Define NFS_R5_CACHE_LINE_BYTES, NFS_R5_L1D_BYTES and NFS_R5_L2_BYTES
     * consistently across translation units to override the 64 B / 32 KiB / 1 MiB defaults.
     */
    template <std::contiguous_iterator It> void sort(It begin, It end) {
        using T = std::iter_value_t<It>;
        static_assert((sizeof(T) == 4 || sizeof(T) == 8) &&
                      (std::is_integral_v<T> || (std::is_floating_point_v<T> && std::numeric_limits<T>::is_iec559)));
        if (begin == end) return;
        T *first = std::to_address(begin), *last = first + (end - begin);
        if constexpr (std::is_floating_point_v<T>) {
            auto is_number = [](T x) { return !std::isnan(x); };
            auto nan = std::find_if_not(first, last, is_number);
            if (nan != last) last = std::partition(nan, last, is_number);
        }
        if (last - first < 32) {
            if (last - first > 1) internal::insertion_sort(first, last);
            return;
        }
        auto general = [](T *a, T *b) {
            const std::size_t n = b - a;
            if (n < 64) {
                std::sort(a, b);
                return;
            }
            if (internal::count_repeated(a, n)) return;
            using Work = internal::Workspace<T>;
            static_assert(sizeof(Work) <= internal::buffer_limit);
            auto work = std::make_unique_for_overwrite<Work>();
            // Sample out-of-order neighbors to choose 8-key batched repair, one-by-one repair, or full distribution.
            if (internal::looks_ordered(a, n)) {
                unsigned breaks = 0;
                const unsigned probes = unsigned(std::min<std::size_t>(256, n - 1));
                const std::size_t step = (n - 1) / probes;
                for (unsigned i = 0; i < probes; ++i) breaks += a[i * step + 1] < a[i * step];
                breaks = breaks * 256 / probes;
                const std::size_t target = internal::slots * 192u / (1 + breaks);
                if (breaks <= 8)
                    internal::split_ordered<true>(a, n, *work, target);
                else
                    internal::split_ordered<false>(a, n, *work, target);
            } else
                internal::distribute(a, n, *work);
        };
        if (!internal::sort_runs(first, last, general)) general(first, last);
    }
}
