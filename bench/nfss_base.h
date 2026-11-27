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
#include <cstring>
#include <random>
#include <vector>

namespace nfs_base {
    namespace internal {
        // Parameters
        // Fragment Size
        static constexpr int CAP = 90;

        // RNG for sampling of min max
        inline std::mt19937 &rng() {
            thread_local std::mt19937 gen{std::random_device{}()};
            return gen;
        }

        // A simple function to swap fragments
        template<typename T>
        static void swap_fragment(T *a, T *b, const int cap, T *tmp) {
            std::memcpy(tmp, a, cap * sizeof(T));
            std::memcpy(a, b, cap * sizeof(T));
            std::memcpy(b, tmp, cap * sizeof(T));
        }

        // Generic Insertion Sort
        template<typename T>
        static void insertion_sort(T *begin, const T *end) {
            if (begin == end) return;

            for (T *i = begin + 1; i != end; ++i) {
                T key = *i;
                T *j = i;

                while (j != begin && *(j - 1) > key) {
                    *j = *(j - 1);
                    --j;
                }
                *j = key;
            }
        }

        // Checks if all elements are the same
        template<typename T>
        static bool check_homogeneity(T *src, const int start, const int end) {
            if (end - start <= 1) return true;

            const T ref = src[start];

            // Cheap early rejects
            if (!(src[end - 1] == ref)) return false;
            const int mid = (start + end - 1) >> 1;
            if (!(src[mid] == ref)) return false;

            // Scan from both ends toward the middle to catch mismatches early
            int i = start + 1;
            int j = end - 2;

            while (i <= j) {
                if (!(src[i] == ref)) return false;
                if (!(src[j] == ref)) return false;
                ++i;
                --j;
            }
            return true;
        }

        // naive scatter of elements into buckets
        template<typename T>
        static std::vector<int> naive_scatter(T *src, const double min, double max, const int bucket_number,
                                              const double inv_interval,
                                              int start, const int end, std::vector<int> &bucket_sizes, T *helper) {
            const double scale = (bucket_number - 1) * inv_interval;
            std::vector bucket_offsets(bucket_number + 1, 0);

            std::fill_n(bucket_sizes.begin(), bucket_number, 0);

            // count bucket sizes, classify elements into buckets
            for (int i = start; i < end; ++i) {
                const auto x = static_cast<double>(src[i]);
                int index = static_cast<int>((x - min) * scale);
                if (index >= bucket_number) index = bucket_number - 1;
                ++bucket_sizes[index];
            }

            int bad_count = 0;

            // bucket sizes -> offsets
            bucket_offsets[0] = 0;
            for (int i = 1; i < bucket_number + 1; ++i) {
                const int bucket_size = bucket_sizes[i - 1];
                bucket_offsets[i] = bucket_offsets[i - 1] + bucket_size;
                if (bucket_size > 99 && bad_count < 401) {
                    bad_count = bad_count + bucket_size;
                }
            }

            // We copy bucket offsets for future use if there are enough big (bad) buckets (otherwise we terminate)
            std::vector<int> bucket_offsets_copy;
            if (bad_count > 499) {
                bucket_offsets_copy.resize(bucket_number + 1);

                std::memcpy(bucket_offsets_copy.data(),
                            bucket_offsets.data(),
                            bucket_offsets.size() * sizeof(int));
            }


            // scatter into helper[0..n)
            for (int i = start; i < end; ++i) {
                const auto x = static_cast<double>(src[i]);
                int index = static_cast<int>((x - min) * scale);
                if (index >= bucket_number) index = bucket_number - 1;
                helper[bucket_offsets[index]++] = src[i];
            }

            // copy back into src[start:end)
            std::memcpy(src + start, helper, static_cast<size_t>(end - start) * sizeof(T));

            return bucket_offsets_copy;
        }

        // IPS4o-inspired bucketed fragment scatter
        template<typename T>
        static std::vector<int> fragment_scatter(
            T *src,
            const double min,
            const double max,
            int bucket_number,
            const double inv_interval,
            int start,
            const int end,
            T *helper,
            int *bucket_end_offset,
            int *bucket_write_off,
            T *tmp,
            std::vector<int> &bucket_sizes,
            std::vector<int> &fragment_sizes,
            const int first_pass,
            double &observed_min,
            double &observed_max
        ) {
            // bucket number depends on if we know exact min/max - first pass tells us we don't
            const int base_bucket_number = bucket_number;
            if (first_pass) bucket_number = base_bucket_number + 2;

            const double scale = (base_bucket_number - 1) * inv_interval;

            if (first_pass) {
                observed_min = min;
                observed_max = max;
            }

            // A function to classify elements into buckets
            auto classify = [&](const T &xv) -> int {
                const auto x = static_cast<double>(xv);

                if (first_pass) {
                    if (x < min) {
                        if (x < observed_min) observed_min = x;
                        return 0;
                    }
                    if (x > max) {
                        if (x > observed_max) observed_max = x;
                        return base_bucket_number + 1;
                    }
                    return static_cast<int>((x - min) * scale) + 1;
                }
                int index = static_cast<int>((x - min) * scale);
                if (index >= base_bucket_number) index = base_bucket_number - 1;
                return index;
            };

            // Coordinates of our helper array based on cap size
            auto frag = [&](const int b) -> T * {
                return helper + static_cast<size_t>(b) * CAP;
            };

            // init helper variables
            std::fill_n(bucket_sizes.begin(), bucket_number, 0);
            std::fill_n(fragment_sizes.begin(), bucket_number, 0);
            int write_pos = start;
            int fragments_written = 0;

            // ------------------------
            // FLUSH FRAGMENTS INTO SRC
            // ------------------------
            for (int i = start; i < end; ++i) {
                const int index = classify(src[i]);
                const int k = fragment_sizes[index]++;

                ++bucket_sizes[index];
                frag(index)[k] = src[i];

                if (k + 1 == CAP) {
                    std::copy(frag(index), frag(index) + CAP, src + write_pos);
                    fragments_written++;
                    write_pos += CAP;
                    fragment_sizes[index] = 0;
                }
            }

            // calculate bucket end offsets
            bucket_end_offset[0] = bucket_sizes[0];
            for (int b = 1; b < bucket_number; ++b) {
                bucket_end_offset[b] = bucket_end_offset[b - 1] + bucket_sizes[b];
            }

            // calculate CAP aligned write offsets
            bucket_write_off[0] = 0;
            for (int b = 1; b < bucket_number; ++b) {
                const int prev_end = bucket_end_offset[b - 1];
                int aligned = ((prev_end + CAP - 1) / CAP) * CAP; // ceil(prev_end/CAP)*CAP
                if (aligned > bucket_end_offset[b]) aligned = bucket_end_offset[b];
                bucket_write_off[b] = aligned;
            }

            // cap-sized fragment helper-function for src
            auto frag_ptr = [&](const int frag_idx) -> T * {
                return src + start + frag_idx * CAP;
            };

            // ------------------------
            // DEFRAGMENTATION BY MOVING FRAGMENTS AND PUTTING THEM IN THE RIGHT PLACE
            // ALSO PUTTING SPILLOVER INTO HELPER
            // ------------------------

            int stored_bucket_idx = 0;

            for (int fragment_idx = 0; fragment_idx < fragments_written; ++fragment_idx) {
                const int cur_fragment_start_off = fragment_idx * CAP;

                while (stored_bucket_idx < bucket_number - 1 &&
                       cur_fragment_start_off >= bucket_end_offset[stored_bucket_idx]) {
                    ++stored_bucket_idx;
                }

                if (cur_fragment_start_off < bucket_write_off[stored_bucket_idx]) {
                    continue;
                }

                const int owner_bucket = classify(src[start + cur_fragment_start_off]);
                const int dest_off = bucket_write_off[owner_bucket];

                if (dest_off < cur_fragment_start_off ||
                    dest_off >= fragments_written * CAP) {
                    if (dest_off + CAP <= bucket_end_offset[owner_bucket]) {
                        std::copy(src + start + cur_fragment_start_off,
                                  src + start + cur_fragment_start_off + CAP,
                                  src + start + dest_off);

                        bucket_write_off[owner_bucket] += CAP;
                    } else {
                        const int fit = bucket_end_offset[owner_bucket] - dest_off; // 0..CAP-1

                        std::copy(src + start + cur_fragment_start_off,
                                  src + start + cur_fragment_start_off + fit,
                                  src + start + dest_off);

                        bucket_write_off[owner_bucket] = bucket_end_offset[owner_bucket];

                        for (int t = fit; t < CAP; ++t) {
                            frag(owner_bucket)[fragment_sizes[owner_bucket]++] =
                                    *(src + start + cur_fragment_start_off + t);
                        }
                    }
                } else {
                    if (owner_bucket == stored_bucket_idx) {
                        const int remaining = bucket_end_offset[stored_bucket_idx] - bucket_write_off[
                                                  stored_bucket_idx];

                        if (remaining < CAP) {
                            std::copy_backward(
                                src + start + cur_fragment_start_off,
                                src + start + cur_fragment_start_off + remaining,
                                src + start + bucket_write_off[stored_bucket_idx] + remaining
                            );

                            for (int t = remaining; t < CAP; ++t) {
                                frag(stored_bucket_idx)[fragment_sizes[stored_bucket_idx]++] =
                                        *(src + start + cur_fragment_start_off + t);
                            }

                            bucket_write_off[stored_bucket_idx] = bucket_end_offset[stored_bucket_idx];
                        } else {
                            if (bucket_write_off[stored_bucket_idx] != cur_fragment_start_off) {
                                std::copy(src + start + cur_fragment_start_off,
                                          src + start + cur_fragment_start_off + CAP,
                                          src + start + bucket_write_off[stored_bucket_idx]);
                            }
                            bucket_write_off[stored_bucket_idx] += CAP;
                        }
                    } else {
                        const int dest_frag_idx = dest_off / CAP;
                        const int incumbent_owner = classify(src[start + bucket_write_off[owner_bucket]]);

                        if (incumbent_owner != owner_bucket) {
                            swap_fragment<T>(frag_ptr(dest_frag_idx), frag_ptr(fragment_idx), CAP, tmp);
                            --fragment_idx;
                        } else {
                            bucket_write_off[owner_bucket] += CAP;
                            --fragment_idx;
                        }
                    }
                }
            }

            // copy bucket offsets for return value
            std::vector<int> bucket_offsets(bucket_number + 1);
            bucket_offsets[0] = 0;
            for (int b = 1; b < bucket_number + 1; ++b) {
                bucket_offsets[b] = bucket_end_offset[b - 1];
            }

            // ------------------------
            // FILL UP WITH REST (SPILLOVER)
            // ------------------------
            for (int b = 0; b < bucket_number; ++b) {
                const int k = fragment_sizes[b];
                if (k == 0) continue;

                int write_off = (b == 0) ? start + bucket_offsets[0] : start + bucket_offsets[b];

                int elm = 0;
                while (elm < k && ((write_off - start) % CAP) != 0) {
                    src[write_off++] = frag(b)[elm++];
                }

                const int tail_start = start + bucket_offsets[b + 1] - (k - elm);
                std::memcpy(src + tail_start, frag(b) + elm, static_cast<size_t>(k - elm) * sizeof(T));
            }

            return bucket_offsets;
        }

        // NFSS RECURSION CORE
        template<typename T>
        static void nfss_core(
            T *src, double min, double max,
            int start, int end, int recursion_counter,
            T *helper,
            int *bucket_end_offset,
            int *bucket_write_off,
            T *tmp,
            std::vector<int> &bucket_sizes,
            std::vector<int> &fragment_sizes,
            int first_pass
        ) {
            // check if elements are the same
            if (!first_pass) {
                if (check_homogeneity(src, start, end)) {
                    return;
                }
            }

            // calculate list basic info
            const int list_size = end - start;
            const double interval = max - min;
            const double inv_interval = 1.0 / interval;

            int bucket_number;
            std::vector<int> bucket_offsets;

            // observed range outside [min, max] (only used on first pass)
            double observed_min = min;
            double observed_max = max;

            // Decide on strategy and bucket number based on list size
            if (list_size > 5000000) {
                bucket_number = 2000;
                bucket_offsets = fragment_scatter(src, min, max, bucket_number, inv_interval, start, end,
                                                  helper, bucket_end_offset, bucket_write_off, tmp, bucket_sizes,
                                                  fragment_sizes, first_pass,
                                                  observed_min, observed_max);
            } else if (list_size > 1000000) {
                bucket_number = 1000;
                bucket_offsets = fragment_scatter(src, min, max, bucket_number, inv_interval, start, end,
                                                  helper, bucket_end_offset, bucket_write_off, tmp, bucket_sizes,
                                                  fragment_sizes, first_pass,
                                                  observed_min, observed_max);
            } else if (list_size > 500000) {
                bucket_number = 200;
                bucket_offsets = fragment_scatter(src, min, max, bucket_number, inv_interval, start, end,
                                                  helper, bucket_end_offset, bucket_write_off, tmp, bucket_sizes,
                                                  fragment_sizes, first_pass,
                                                  observed_min, observed_max);
            } else if (list_size > 10000) {
                bucket_number = 100;
                bucket_offsets = fragment_scatter(src, min, max, bucket_number, inv_interval, start, end,
                                                  helper, bucket_end_offset, bucket_write_off, tmp, bucket_sizes,
                                                  fragment_sizes, first_pass,
                                                  observed_min, observed_max);
            } else {
                bucket_number = std::min(6500, list_size);
                bucket_offsets = naive_scatter(src, min, max, bucket_number, inv_interval, start, end, bucket_sizes,
                                               helper);
                if (bucket_offsets.empty()) {
                    return;
                }
            }

            // handle out-of-range buckets on first pass (on first pass we don't know exact min/max)
            if (first_pass) {
                if (observed_min < min) {
                    nfss_core(src, observed_min, min,
                              start + bucket_offsets[0],
                              start + bucket_offsets[1],
                              recursion_counter, helper, bucket_end_offset, bucket_write_off, tmp,
                              bucket_sizes,
                              fragment_sizes, 0);
                }
                if (observed_max > max) {
                    nfss_core(src, max, observed_max,
                              start + bucket_offsets[bucket_number + 1],
                              start + bucket_offsets[bucket_number + 2],
                              recursion_counter, helper, bucket_end_offset, bucket_write_off, tmp,
                              bucket_sizes, fragment_sizes, 0);
                }
            }

            // calculate interval size and "bad" critical mass
            const double intervalSize = interval / (bucket_number - 1);
            const double critical_mass = 0.95 * list_size;

            // decide what to do with each bucket based on bucket size (on first pass we have two extra buckets)
            for (int i = 0; i < bucket_number; i++) {
                if (first_pass) i = i + 1;

                const int bucket_start = start + bucket_offsets[i];
                const int bucket_end = start + bucket_offsets[i + 1];
                const int bucket_size = bucket_end - bucket_start;

                if (first_pass) i = i - 1;

                // insertion sort at the end will handle these buckets
                if (bucket_size <= 16) continue;

                // pathological buckets that have hardly lost any mass (bucket size is close to list size)
                if (bucket_size > critical_mass) {
                    recursion_counter++;
                    // we try a couple of times with recursion, and if it doesn't work we fall back on std::sort
                    if (recursion_counter < 3) {
                        nfss_core(src,
                                  min + intervalSize * i,
                                  min + intervalSize * (i + 1),
                                  bucket_start, bucket_end,
                                  recursion_counter, helper, bucket_end_offset, bucket_write_off, tmp, bucket_sizes,
                                  fragment_sizes,
                                  0);
                    } else {
                        std::sort(src + bucket_start, src + bucket_end);
                    }
                } else {
                    // we recurse on the bucket, propagating the min and max analytically
                    nfss_core(src,
                              min + intervalSize * i,
                              min + intervalSize * (i + 1),
                              bucket_start, bucket_end,
                              0, helper, bucket_end_offset, bucket_write_off, tmp,
                              bucket_sizes, fragment_sizes, 0);
                }
            }
        }
    }

    // MAIN FUNCTION
    template<typename It>
    void sort(It begin, It end) {
        using T = std::iterator_traits<It>::value_type;
        static_assert(std::is_arithmetic_v<T>, "sort supports only arithmetic types.");
        static_assert(std::contiguous_iterator<It>,
                      "sort requires contiguous iterators (vector/array/span).");

        if (begin == end) return;


        // Check if the data is already sorted
        if (*(end - 1) >= *begin && std::is_sorted(begin, end)) {
            return;
        }

        // Check if the data is sorted in descending order
        if (*(end - 1) <= *begin) {
            auto is_reverse_sorted = true;

            for (auto i = begin; i != end - 1; ++i) {
                if (*i < *(i + 1)) {
                    is_reverse_sorted = false;
                    break;
                }
            }

            if (is_reverse_sorted) {
                std::reverse(begin, end);
                return;
            }
        }


        T *begin_adr = std::to_address(begin);
        T *end_adr = std::to_address(end);


        const int data_size = static_cast<int>(end_adr - begin_adr);

        // calculate min/max - Approximation is used if there are more than 10000 elements
        double mn, mx;
        int first_pass;

        if (data_size > 10000) {
            double approx_min = std::numeric_limits<double>::infinity();
            double approx_max = -std::numeric_limits<double>::infinity();

            auto &rng = internal::rng();
            std::uniform_int_distribution<size_t> dist(0, static_cast<size_t>(data_size) - 1);

            const size_t samples = std::max<size_t>(1000, static_cast<size_t>(data_size) / 1000);
            for (size_t i = 0; i < samples; ++i) {
                auto v = static_cast<double>(begin_adr[dist(rng)]);
                approx_min = std::min(approx_min, v);
                approx_max = std::max(approx_max, v);
            }

            if (approx_min != approx_max) {
                mn = approx_min;
                mx = approx_max;
                first_pass = 1;
            } else {
                auto [mn_it, mx_it] = std::minmax_element(begin_adr, end_adr);
                mn = static_cast<double>(*mn_it);
                mx = static_cast<double>(*mx_it);
                first_pass = 0;
            }
        } else {
            auto [mn_it, mx_it] = std::minmax_element(begin_adr, end_adr);
            mn = static_cast<double>(*mn_it);
            mx = static_cast<double>(*mx_it);
            first_pass = 0;
        }

        // initialize utility and helper arrays. We carry them through the recursion and reuse them
        // 6500 is the maximum number of buckets for naive scatter
        // 2002 is the maximum number of buckets for fragment scatter
        std::vector bucket_sizes(6500, 0);
        std::vector fragment_sizes(2002, 0);

        auto *helper = new T[2002 * internal::CAP];
        auto *bucket_end_offset = new int[2002];
        auto *bucket_write_off = new int[2002];
        auto *tmp = new T[internal::CAP];

        // Start of recursion
        internal::nfss_core<T>(begin_adr, mn, mx,
                               0, data_size, 0,
                               helper, bucket_end_offset, bucket_write_off, tmp,
                               bucket_sizes, fragment_sizes, first_pass);


        // final clean up after recursive bucketing is done
        internal::insertion_sort(begin_adr, end_adr);

        delete[] helper;
        delete[] bucket_end_offset;
        delete[] bucket_write_off;
        delete[] tmp;
    }
}
