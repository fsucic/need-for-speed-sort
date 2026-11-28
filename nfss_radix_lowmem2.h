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
#include <numeric>
#include <type_traits>
#include <utility>

#ifndef NFS_LOWMEM2_CACHE_LINE_BYTES
#define NFS_LOWMEM2_CACHE_LINE_BYTES 64
#endif
#ifndef NFS_LOWMEM2_L1D_BYTES
#define NFS_LOWMEM2_L1D_BYTES (32 * 1024)
#endif
#ifndef NFS_LOWMEM2_L2_BYTES
#define NFS_LOWMEM2_L2_BYTES (1024 * 1024)
#endif

namespace nfs_radix_lowmem2 {
    namespace internal {
        constexpr std::size_t line_bytes = NFS_LOWMEM2_CACHE_LINE_BYTES;
        constexpr std::size_t l1_bytes = NFS_LOWMEM2_L1D_BYTES;
        constexpr std::size_t l2_bytes = NFS_LOWMEM2_L2_BYTES;
        static_assert(std::has_single_bit(line_bytes) && line_bytes >= 8);
        static_assert(l1_bytes >= line_bytes && l2_bytes >= l1_bytes);
        constexpr std::size_t buffer_limit = 896 * 1024;
        constexpr std::size_t merge_bytes = std::min(buffer_limit, l2_bytes);
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

        template <typename T> static auto order_key(T value) {
            if constexpr (std::is_integral_v<T>) {
                using U = std::make_unsigned_t<T>;
                if constexpr (std::is_signed_v<T>)
                    return U(U(value) ^ (U(1) << (std::numeric_limits<U>::digits - 1)));
                else
                    return U(value);
            } else {
                using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
                constexpr U sign = U(1) << (std::numeric_limits<U>::digits - 1);
                const U bits = std::bit_cast<U>(value);
                return bits ^ ((bits & sign) ? ~U(0) : sign);
            }
        }

        template <typename T> static auto key_bits(T value) {
            using U = std::conditional_t<sizeof(T) == 8, std::uint64_t, std::uint32_t>;
            return std::bit_cast<U>(value);
        }

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

        constexpr unsigned slots = 1u << 16;
        constexpr unsigned histogram_bits = 15;
        constexpr unsigned digit_bits = std::clamp<unsigned>(std::bit_width(l2_bytes / (8 * line_bytes)), 8, 11);
        constexpr unsigned max_bins = 1u << digit_bits;
        template <typename T> constexpr unsigned block_size = std::min<std::size_t>(32, 4 * (line_bytes / sizeof(T)));
        template <typename T> struct Workspace {
            T items[slots + 64];
            T overflow[slots / 32];
            std::uint64_t bitmap[slots / 64 + 1];
            std::uint16_t counts[1u << (histogram_bits + 1)];
            unsigned used[max_bins];
            std::size_t next[max_bins];
            std::size_t ends[1 + (sizeof(T) * 8 + 7) / 8][max_bins];
        };
        template <typename T>
        constexpr unsigned leaf_size = std::clamp<std::size_t>(l2_bytes / (4 * sizeof(T)), 256, slots - 1);
        template <typename T, typename Classify>
        static void fragment_scatter(T *data, std::size_t n, unsigned bins, Classify classify, Workspace<T> &ws,
                                     std::size_t *ends) {
            constexpr unsigned block_size = internal::block_size<T>;
            static_assert(max_bins * block_size <= slots);
            auto *used = ws.used;
            auto *buffers = ws.items;
            auto *next = ws.next;
            std::fill_n(used, bins, 0u);
            std::fill_n(ends, bins, std::size_t(0));
            std::size_t written = 0;
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

        template <typename T, typename Classify>
        static bool sparse_leaf(T *data, std::size_t n, Classify classify, unsigned bins, Workspace<T> &ws) {
            auto *taken = ws.bitmap;
            const unsigned words = (bins + 63) / 64 + 1;
            std::fill_n(taken, words, UINT64_C(0));
            T *items = ws.items;
            auto *overflow = ws.overflow;
            unsigned spilled = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const unsigned bin = classify(data[i]);
                unsigned word = bin >> 6;
                auto free = ~taken[word] & (~UINT64_C(0) << (bin & 63));
                if (!free) {
                    free = ~taken[++word];
                    if (!free) {
                        if (spilled >= n / 32) return false;
                        overflow[spilled++] = data[i];
                        continue;
                    }
                }
                const unsigned bit = std::countr_zero(free);
                taken[word] |= UINT64_C(1) << bit;
                items[word * 64 + bit] = data[i];
            }
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

        template <typename T, typename U, typename Read>
        static void radix_leaf(T *data, std::size_t n, Workspace<T> &ws, U base, U span, int half, Read read) {
            const unsigned lower_bins = 1u << half;
            const unsigned upper_bins = unsigned(span >> half) + 1;
            const unsigned mask = lower_bins - 1;
            auto *lower = ws.counts;
            auto *upper = lower + lower_bins;
            std::fill_n(lower, lower_bins + upper_bins, 0u);
            for (std::size_t i = 0; i < n; ++i) {
                const U x = U(order_key(read(i)) - base);
                ++lower[x & mask];
                ++upper[x >> half];
            }
            std::exclusive_scan(lower, lower + lower_bins, lower, 0u);
            std::exclusive_scan(upper, upper + upper_bins, upper, 0u);
            auto *out = ws.items;
            for (std::size_t i = 0; i < n; ++i) {
                const T value = read(i);
                const U x = U(order_key(value) - base);
                out[lower[x & mask]++] = value;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const U x = U(order_key(out[i]) - base);
                data[upper[x >> half]++] = out[i];
            }
        }

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

        template <bool Batched, typename T, typename Sort>
        static bool repair_order(T *data, std::size_t n, Workspace<T> &ws, Sort sort_extra) {
            auto *extra = ws.items;
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
                std::copy_n(kept, count, extra);
            }
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

        template <typename T> using Key = decltype(order_key(T{}));

        template <typename T, typename Map>
        static void bucket_starts(const T *data, std::size_t n, unsigned bins, Map position, std::size_t *ends) {
            std::fill_n(ends, bins, std::size_t(0));
            for (std::size_t i = 0; i < n; ++i) ++ends[position(data[i])];
            std::exclusive_scan(ends, ends + bins, ends, std::size_t(0));
        }

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
        template <typename T, typename SortBucket>
        static bool magnitude_partition(T *data, std::size_t n, unsigned bins, bool leaf, Workspace<T> &ws,
                                        SortBucket sort_bucket) {
            using U = Key<T>;
            constexpr unsigned digits = std::numeric_limits<U>::digits;
            constexpr U sign_bit = U(1) << (digits - 1);
            constexpr unsigned samples = 128;
            const auto stride = n / samples;
            std::uint32_t state = 73;
            unsigned signs = 0;
            U origin = ~U(0), largest = 0;
            for (unsigned i = 0; i < samples; ++i) {
                state = 1664525u * state + 1013904223u;
                const auto raw = key_bits(data[i * stride + ((std::uint64_t(state) * stride) >> 32)]);
                const U magnitude = raw & (sign_bit - 1);
                signs |= 1u << (raw >> (digits - 1));
                origin = std::min(origin, magnitude);
                largest = std::max(largest, magnitude);
            }
            if (signs != 3) return false;
            const unsigned half = bins / 2;
            const auto shift =
                unsigned(std::max(0, int(std::bit_width(U(largest - origin))) - int(std::bit_width(half - 1))));
            const auto position = [=](T x) {
                const auto raw = key_bits(x);
                const U magnitude = raw & (sign_bit - 1);
                const auto distance = U(std::max(magnitude, origin) - origin);
                const auto band = unsigned(std::min<U>(distance >> shift, half - 1));
                const auto reverse = 0u - unsigned(raw >> (digits - 1));
                return (half + band) ^ (reverse & (bins - 1));
            };
            if (leaf) return sparse_leaf(data, n, position, bins, ws);
            auto *ends = ws.ends[0];
            fragment_scatter(data, n, bins, position, ws, ends);
            std::size_t first = 0;
            for (unsigned b = 0; b < bins; ++b) {
                const bool negative = b < half;
                const unsigned slot = negative ? half - 1 - b : b - half;
                const auto count = ends[b] - first;
                if (count > 1) {
                    if (!slot || slot + 1 == half || count > n / 4) {
                        sort_bucket(data + first, count, U(1), U(0));
                    } else {
                        const U lo = origin + (U(slot) << shift);
                        const U hi = lo + std::min<U>((U(1) << shift) - 1, sign_bit - 1 - lo);
                        const U lower = negative ? U((sign_bit - 1) - hi) : U(sign_bit + lo);
                        const U upper = negative ? U((sign_bit - 1) - lo) : U(sign_bit + hi);
                        sort_bucket(data + first, count, lower, upper);
                    }
                }
                first = ends[b];
            }
            return true;
        }

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
        template <typename T>
        static void distribute(T *data, std::size_t n, Workspace<T> &ws, unsigned depth = 0, Key<T> base = 1,
                               Key<T> high_key = 0) {
            if (n < 32) {
                if (n > 1) insertion_sort(data, data + n);
                return;
            }
            const bool leaf = n <= leaf_size<T>;
            constexpr auto target = std::clamp<std::size_t>(l1_bytes / (2 * sizeof(T)), 256, slots / 4);
            const auto desired = std::clamp<std::size_t>(1 + (n - 1) / target, 256, max_bins);
            const unsigned fanout = std::bit_ceil(unsigned(desired));
            const unsigned bins = leaf ? std::min(slots, std::bit_ceil(unsigned(n)) * 4) : fanout;
            if constexpr (std::is_floating_point_v<T>) {
                if (!depth && n >= 1024) {
                    const auto sort_bucket = [&](T *first, std::size_t count, Key<T> lo, Key<T> hi) {
                        distribute(first, count, ws, 1, lo, hi);
                    };
                    if (magnitude_partition(data, n, bins, leaf, ws, sort_bucket)) return;
                }
            }
            const bool inherited = base <= high_key;
            T low{}, high{};
            if (!inherited) {
                auto bounds = extrema(data, n);
                low = bounds.first;
                high = bounds.second;
                if (low == high) return;
                if constexpr (std::is_floating_point_v<T>) {
                    // Keep exact encoded bounds even when the extrema compare equal to the other signed zero.
                    if (low == 0 && !std::signbit(low) &&
                        std::any_of(data, data + n, [](T x) { return std::signbit(x); }))
                        low = -T(0);
                    if (high == 0 && std::signbit(high) &&
                        std::any_of(data, data + n, [](T x) { return !std::signbit(x); }))
                        high = T(0);
                }
                base = order_key(low);
                high_key = order_key(high);
            }
            const auto span = high_key - base;
            if (!span) return;
            const int width = std::bit_width(span);

            if constexpr (std::is_integral_v<T>) {
                if (!depth && n >= 1024) {
                    std::array<Key<T>, 64> sample;
                    const std::size_t stride = n / sample.size();
                    std::uint32_t random = 73;
                    for (unsigned i = 0; i < sample.size(); ++i) {
                        random = 1664525u * random + 1013904223u;
                        sample[i] = order_key(data[i * stride + ((std::uint64_t(random) * stride) >> 32)]);
                    }
                    std::sort(sample.begin(), sample.end());
                    const auto a = sample[2], b = sample[sample.size() - 3];
                    const auto spread = b - a;
                    if (int(std::bit_width(spread)) + 5 < width) {
                        const auto lo = a - std::min(spread, Key<T>(a - base));
                        const auto hi = b + std::min(spread, Key<T>(high_key - b));
                        T *left = data, *right = data + n;
                        if (lo != base) left = std::partition(left, right, [=](T x) { return order_key(x) < lo; });
                        if (hi != high_key)
                            right = std::partition(left, right, [=](T x) { return order_key(x) <= hi; });
                        distribute(data, left - data, ws, depth + 1);
                        distribute(left, right - left, ws, depth + 1, lo, hi);
                        distribute(right, data + n - right, ws, depth + 1);
                        return;
                    }
                }
            }
            if constexpr (std::is_integral_v<T>) {
                if (!leaf && span < slots && span < n && n <= std::uintmax_t(std::numeric_limits<T>::max())) {
                    const unsigned values = unsigned(span) + 1;
                    std::fill_n(ws.items, values, T(0));
                    for (std::size_t i = 0; i < n; ++i) ++ws.items[order_key(data[i]) - base];
                    T *out = data;
                    for (unsigned b = 0; b < values; ++b)
                        out = std::fill_n(out, ws.items[b], T((base + b) ^ order_key(T(0))));
                    return;
                }
            }
            if (leaf) {
                if constexpr (std::is_integral_v<T>) {
                    if (span < 4 * n && span < std::size(ws.counts)) {
                        const auto values = unsigned(span) + 1;
                        std::fill_n(ws.counts, values, 0u);
                        for (std::size_t i = 0; i < n; ++i) ++ws.counts[order_key(data[i]) - base];
                        T *out = ws.items;
                        for (unsigned b = 0; b < values; ++b) {
                            const T value = T((base + b) ^ order_key(T(0)));
                            const unsigned count = ws.counts[b];
                            out[0] = out[1] = value;
                            if (count > 2) std::fill_n(out + 2, count - 2, value);
                            out += count;
                        }
                        std::copy_n(ws.items, n, data);
                        return;
                    }
                }
                const int half = width / 2;
                if (width <= 2 * int(histogram_bits) && (1u << (width - half)) <= 4 * n) {
                    radix_leaf(data, n, ws, base, span, half, [&](std::size_t i) { return data[i]; });
                    return;
                }
                const int shift = std::max(0, width - int(std::bit_width(bins - 1)));
                auto position = [=](T x) { return unsigned((order_key(x) - base) >> shift); };
                if (sparse_leaf(data, n, position, unsigned(span >> shift) + 1, ws)) return;
                if (inherited) {
                    distribute(data, n, ws, depth);
                    return;
                }

                if (n >= 256 && width > 1 && depth + 1 < std::size(ws.ends)) {
                    const unsigned target = std::clamp(unsigned(n) / 64, 4u, 256u);
                    const unsigned shift = unsigned(std::max(0, width - int(std::bit_width(target - 1))));
                    const unsigned groups = unsigned(span >> shift) + 1;
                    std::array<unsigned, 257> offsets{};
                    const auto group = [=](T x) { return unsigned((order_key(x) - base) >> shift); };
                    for (std::size_t i = 0; i < n; ++i) ++offsets[group(data[i]) + 1];
                    std::partial_sum(offsets.begin(), offsets.begin() + groups + 1, offsets.begin());
                    std::array<unsigned, 256> next;
                    std::copy_n(offsets.begin(), groups, next.begin());
                    for (std::size_t i = 0; i < n; ++i) ws.items[next[group(data[i])]++] = data[i];
                    std::copy_n(ws.items, n, data);
                    if (!shift) return;
                    for (unsigned b = 0; b < groups; ++b) {
                        const auto count = offsets[b + 1] - offsets[b];
                        if (count < 2) continue;
                        const auto lo = Key<T>(base + (Key<T>(b) << shift));
                        const auto hi = Key<T>(lo + std::min<Key<T>>((Key<T>(1) << shift) - 1, high_key - lo));
                        distribute(data + offsets[b], count, ws, depth + 1, lo, hi);
                    }
                    return;
                }
                const unsigned groups = std::bit_ceil(unsigned(n)) / 2;
                const int group_shift = std::max(0, width - int(std::bit_width(groups - 1)));
                auto group = [=](T x) { return unsigned((order_key(x) - base) >> group_shift); };
                const unsigned used = unsigned(span >> group_shift) + 1;
                std::fill_n(ws.counts, used, 0u);
                for (std::size_t i = 0; i < n; ++i) ++ws.counts[group(data[i])];
                std::exclusive_scan(ws.counts, ws.counts + used, ws.counts, 0u);
                for (std::size_t i = 0; i < n; ++i) ws.items[ws.counts[group(data[i])]++] = data[i];
                if (group_shift) {
                    unsigned start = 0;
                    for (unsigned b = 0; b < used; ++b) {
                        const unsigned end = ws.counts[b];
                        if (end - start > 32) std::sort(ws.items + start, ws.items + end);
                        start = end;
                    }
                    insertion_sort(ws.items, ws.items + n);
                }
                std::copy_n(ws.items, n, data);
                return;
            }
            if (depth == std::size(ws.ends)) {
                std::sort(data, data + n);
                return;
            }
            int shift = std::max(0, width - int(std::bit_width(bins - 1)));
            if constexpr (std::is_integral_v<T>) {
                if (span / 2 < n && width > 1) {
                    const unsigned target = unsigned(std::min<std::size_t>(slots - 1, l1_bytes / sizeof(T)));
                    shift = std::max(shift, std::min(width - 1, int(std::bit_width(target) - 1)));
                }
            }
            auto position = [=](T x) { return unsigned((order_key(x) - base) >> shift); };
            const unsigned used = unsigned(span >> shift) + 1;
            auto *ends = ws.ends[depth];
            bool half_staged = false, packed_staged = false;
            T low_value{};
            if constexpr (std::is_integral_v<T>) {
                low_value = T(base ^ order_key(T(0)));
                half_staged = (std::endian::native == std::endian::little || std::endian::native == std::endian::big) &&
                              span <= std::numeric_limits<Half<T>>::max() && span / 512 < n;
                if (half_staged)
                    half_scatter(data, n, used, low_value, position, ends);
                else {
                    constexpr int digits = std::numeric_limits<Key<T>>::digits;
                    packed_staged = shift && width < digits && width + shift <= digits;
                    if (packed_staged)
                        PackedRange<T>(unsigned(width), unsigned(shift))
                            .scatter(data, n, used, low_value, position, ends);
                }
            }
            if (!half_staged && !packed_staged) fragment_scatter(data, n, used, position, ws, ends);
            if (!shift) {
                if constexpr (std::is_integral_v<T>) {
                    if (half_staged)
                        for (std::size_t j = 0; j < n; ++j) data[j] = staged_value(data + j, low_value);
                }
                return;
            }
            std::size_t first = 0;
            for (unsigned b = 0; b < used; ++b) {
                const auto child = ends[b] - first;
                if constexpr (std::is_integral_v<T>) {
                    if (packed_staged && child) {
                        const auto origin = T((base + (Key<T>(b) << shift)) ^ order_key(T(0)));
                        const PackedRange<T> packed(unsigned(width), static_cast<unsigned>(shift));
                        for (std::size_t i = first; i < ends[b]; ++i) data[i] = packed.read(data + i, origin);
                    } else if (half_staged) {
                        T *start = data + first;
                        if (child >= 32 && child <= leaf_size<T> && child <= n / 4) {
                            const auto lo = Key<T>(base + (Key<T>(b) << shift));
                            const auto extent = std::min<Key<T>>((Key<T>(1) << shift) - 1, high_key - lo);
                            const int child_bits = std::bit_width(extent), half = child_bits / 2;
                            if (extent >= std::min<std::size_t>(4 * child, slots) &&
                                child_bits <= 2 * int(histogram_bits) && (1u << (child_bits - half)) <= 4 * child) {
                                radix_leaf(start, child, ws, lo, extent, half,
                                           [&](std::size_t j) { return staged_value(start + j, low_value); });
                                first = ends[b];
                                continue;
                            }
                        }
                        for (std::size_t j = 0; j < child; ++j) start[j] = staged_value(start + j, low_value);
                    }
                }
                if (child > n / 4)
                    distribute(data + first, child, ws, depth + 1);
                else if (child > 1) {
                    const auto lo = Key<T>(base + (Key<T>(b) << shift));
                    const auto hi = Key<T>(lo + std::min<Key<T>>((Key<T>(1) << shift) - 1, high_key - lo));
                    distribute(data + first, child, ws, depth + 1, lo, hi);
                }
                first = ends[b];
            }
        }

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
     * Sort a writable contiguous range of 32/64-bit integers or IEEE floats by radix distribution.
     * NaNs follow all numbers. The signs of zeros and the original NaN payloads are preserved.
     * Unstable. Uses at most 896 KiB of heap scratch per call and O(log n) stack space.
     * Define NFS_LOWMEM2_CACHE_LINE_BYTES, NFS_LOWMEM2_L1D_BYTES and NFS_LOWMEM2_L2_BYTES
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
