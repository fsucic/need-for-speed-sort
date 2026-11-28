// NFS Sort benchmark, rebuilt from the protocol in arXiv:2607.11850 §5:
// every algorithm sorts the same input in each run, only the sort call is timed
// (steady_clock), and every output is validated.
//
//   nfsbench --dist uniform --n 1000000 [--type u64|i64|u32|f64] [--runs 10]
//            [--algos nfss,std,pdq,ips4o,ips2ra,spread,bls]  (also: nfss_base = original nfss.h) [--seed 42] [--data DIR]
//
// CSV rows on stdout: dist,type,n,algo,run,ns. A median summary goes to stderr.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <pthread.h>

#include "nfss.h"
#include "nfss_r5.h"
#include "nfss_radix_lowmem2.h"
#include "nfss_base.h"  // frozen copy of the original nfss.h (namespace nfs_base) for same-run before/after
#ifdef NFS_BENCH_RADIX
#include "nfs_radix.h"
#endif
#ifdef NFS_BENCH_CLASSIC
#include "nfs_classic.h"
#endif
#include "pdqsort.h"
// Sequential ips4o only: Apple headers define _REENTRANT, which would pull in its TBB parallel path.
#undef _REENTRANT
#include "ips4o.hpp"
#include "ips2ra.hpp"
#include <boost/sort/spreadsort/spreadsort.hpp>
// BLS relies on glibc's `uint` typedef and GCC VLAs (build with g++).
using uint = unsigned int;
#include "bls.h"

using Clock = std::chrono::steady_clock;

struct Options {
    std::string dist = "uniform";
    std::string type = "u64";
    size_t n = 1'000'000;
    int runs = 10;
    bool check_only = false;
    bool last_input = false;
    uint64_t seed = 42;
    std::string algos = "nfss,nfss_base";
    std::string data_dir = "data";
};

[[noreturn]] static void die(const std::string &msg) {
    std::cerr << "nfsbench: " << msg << "\n";
    std::exit(2);
}

static std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, sep);)
        if (!item.empty()) out.push_back(item);
    return out;
}

// ---------------------------------------------------------------------------
// Datasets
// ---------------------------------------------------------------------------

// SOSD binary format: uint64 count, then count uint64 keys.
static const std::map<std::string, std::string> kSosdFiles = {
    {"books", "books_800M_uint64"},
    {"fb", "fb_200M_uint64"},
    {"osm", "osm_cellids_800M_uint64"},
    {"wiki", "wiki_ts_200M_uint64"},
};

// Holds a full SOSD dataset; each sample is a fresh partial Fisher-Yates draw of n keys,
// which matches "shuffle, then take the first n" without reshuffling the whole file.
class SosdPool {
public:
    SosdPool(const std::string &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) die("cannot open " + path + " (run scripts/download_sosd.sh)");
        uint64_t count = 0;
        in.read(reinterpret_cast<char *>(&count), sizeof count);
        keys_.resize(count);
        in.read(reinterpret_cast<char *>(keys_.data()), count * sizeof(uint64_t));
        if (!in) die("short read on " + path);
    }
    template<typename T>
    void sample(std::vector<T> &out, size_t n, std::mt19937_64 &rng, bool last_input) {
        if (n > keys_.size()) die("n exceeds dataset size " + std::to_string(keys_.size()));
        // Replay the swaps backwards so a cell has the same sample when run or resumed alone.
        std::vector<size_t> picks(last_input ? 0 : n);
        for (size_t i = 0; i < n; ++i) {
            std::uniform_int_distribution<size_t> pick(i, keys_.size() - 1);
            const auto j = pick(rng);
            if (!last_input) picks[i] = j;
            std::swap(keys_[i], keys_[j]);
        }
        out.assign(keys_.begin(), keys_.begin() + n);
        for (size_t i = picks.size(); i-- > 0;) std::swap(keys_[i], keys_[picks[i]]);
    }

private:
    std::vector<uint64_t> keys_;
};

// Synthetic generators produce doubles in a natural range; to_key maps them onto the key type.
// Integer types are scaled so the values span a wide range rather than collapsing to a few ints.
template <typename T>
static T to_key(double x) {
    if constexpr (std::is_floating_point_v<T>) return static_cast<T>(x);
    else {
        constexpr double hi = static_cast<double>(std::numeric_limits<T>::max()) * 0.999;
        constexpr double lo = std::is_signed_v<T> ? -hi : 0.0;
        return static_cast<T>(std::clamp(x, lo, hi));
    }
}

template <typename T>
static double key_span() {
    // Synthetic distributions are laid out on [0, span).
    if constexpr (std::is_floating_point_v<T>) return 1.0;
    else if constexpr (sizeof(T) == 4) return 4e9 / (std::is_signed_v<T> ? 2 : 1);
    else return 1.8e19 / (std::is_signed_v<T> ? 2 : 1);
}

template <typename T>
static void generate(const Options &o, std::vector<T> &v, std::mt19937_64 &rng, SosdPool *pool) {
    const size_t n = o.n;
    const double span = key_span<T>();
    v.resize(n);
    std::uniform_real_distribution<double> unif(0.0, 1.0);

    auto fill = [&](auto &&draw) { for (auto &x : v) x = to_key<T>(draw()); };
    auto sorted_uniform = [&] { fill([&] { return unif(rng) * span; }); std::sort(v.begin(), v.end()); };

    const std::string &d = o.dist;
    if (pool) {
        pool->sample(v, n, rng, o.last_input);
        return;
    }
    if (d == "edge_equal" || d == "edge_two" || d == "edge_top" || d == "edge_bottom") {
        using U = std::conditional_t<sizeof(T) == 8, uint64_t, uint32_t>;
        U a = std::bit_cast<U>(T(1));
        U b = std::bit_cast<U>(T(37));
        if (d == "edge_equal") b = a;
        if (d == "edge_top") b = a ^ (U(1) << (8 * sizeof(T) - 1));
        if (d == "edge_bottom") b = a ^ U(1);
        for (T &x : v) x = std::bit_cast<T>(rng() & 1 ? a : b);
        return;
    }
    if (d == "edge_ieee" || d == "edge_wide" || d == "edge_denorm" || d == "edge_nan") {
        if constexpr (std::is_same_v<T, double>) {
            const std::array<uint64_t, 14> raw = {
                0, 0x8000000000000000, 1, 0x8000000000000001, 0x000fffffffffffff,
                0x800fffffffffffff, 0x7fefffffffffffff, 0xffefffffffffffff, 0x7ff0000000000000,
                0xfff0000000000000, 0x7ff8000000001234, 0xfff8000000005678, 0x7ff0000000000001,
                0xfff0000000000021};
            for (auto &x : v) {
                uint64_t bits = raw[rng() % raw.size()];
                if (d == "edge_nan") bits = raw[10 + rng() % 4];
                if (d == "edge_denorm") bits = rng() & 0x800fffffffffffffULL;
                if (d == "edge_wide") {
                    bits = rng();
                    if ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
                        bits ^= 0x0010000000000000ULL;
                }
                x = std::bit_cast<double>(bits);
            }
            return;
        } else die("IEEE edge inputs require --type f64");
    }
    if (d == "uniform") fill([&] { return unif(rng) * span; });
    else if (d == "sorted") sorted_uniform();
    else if (d == "reverse") { sorted_uniform(); std::reverse(v.begin(), v.end()); }
    else if (d == "organpipe") { sorted_uniform(); std::reverse(v.begin() + n / 2, v.end()); }
    else if (d == "nearlysorted") {
        // sorted, then 1% of positions swapped with a random partner
        sorted_uniform();
        std::uniform_int_distribution<size_t> pos(0, n - 1);
        for (size_t i = 0; i < n / 100; ++i) std::swap(v[pos(rng)], v[pos(rng)]);
    } else if (d == "normal") {
        std::normal_distribution<double> g(0.5, 0.1);
        fill([&] { return g(rng) * span; });
    } else if (d == "exponential") {
        std::exponential_distribution<double> e(10.0);
        fill([&] { return e(rng) * span; });
    } else if (d == "fewunique") {
        // 100 distinct values
        std::vector<T> values(100);
        for (auto &x : values) x = to_key<T>(unif(rng) * span);
        std::uniform_int_distribution<size_t> pick(0, values.size() - 1);
        for (auto &x : v) x = values[pick(rng)];
    } else if (d == "clustered") {
        // 10 tight Gaussian clusters at random centres
        std::vector<double> centres(10);
        for (auto &c : centres) c = unif(rng) * span;
        std::uniform_int_distribution<size_t> pick(0, centres.size() - 1);
        std::normal_distribution<double> g(0.0, span * 1e-4);
        fill([&] { return centres[pick(rng)] + g(rng); });
    }
    // SOSD's generated distributions (gen_norm.py / gen_uniform.py), drawn i.i.d. instead of by quantiles.
    else if (d == "sosd_normal") {
        std::normal_distribution<double> g(0.0, 1.0);
        fill([&] { return (std::clamp(g(rng), -6.0, 6.0) + 6.0) / 12.0 * span; });
    } else if (d == "sosd_lognormal") {
        std::lognormal_distribution<double> g(0.0, 2.0);
        fill([&] { return std::min(g(rng) / std::exp(12.0), 1.0) * span; });
    } else if (d == "sosd_dense") {
        std::uniform_int_distribution<uint64_t> g(0, 2 * n);
        fill([&] { return static_cast<double>(g(rng)); });
    } else if (d == "sosd_sparse") fill([&] { return unif(rng) * span; });
    else if (d == "dup300" || d == "dup1000" || d == "dup10000") {
        std::vector<T> values(std::stoul(d.substr(3)));
        for (T &value : values) value = to_key<T>(unif(rng) * span);
        std::uniform_int_distribution<size_t> pick(0, values.size() - 1);
        for (T &value : v) value = values[pick(rng)];
    } else if (d == "organpipe_overlap" || d == "organpipe_mirror") {
        fill([&] { return unif(rng) * span; });
        if (d == "organpipe_overlap") {
            std::sort(v.begin(), v.begin() + n / 2);
            std::sort(v.begin() + n / 2, v.end(), std::greater<T>{});
        } else {
            std::sort(v.begin(), v.begin() + (n + 1) / 2);
            std::reverse_copy(v.begin(), v.begin() + n / 2, v.end() - n / 2);
        }
    } else if (d == "runs2" || d == "runs16" || d == "runs1000keys") {
        fill([&] { return unif(rng) * span; });
        const size_t runs = d == "runs2" ? 2 : 16;
        const size_t length = d == "runs1000keys" ? 1000 : (n + runs - 1) / runs;
        for (size_t i = 0; i < n; i += length)
            std::sort(v.begin() + i, v.begin() + std::min(i + length, n));
    } else if (d == "tail1" || d == "tail10" || d == "tail40") {
        fill([&] { return unif(rng) * span; });
        const size_t tail = n * std::stoul(d.substr(4)) / 100;
        std::sort(v.begin(), v.end() - tail);
    } else if (d == "nearlysorted5") {
        sorted_uniform();
        std::uniform_int_distribution<size_t> pos(0, n - 1);
        for (size_t i = 0; i < n / 20; ++i) std::swap(v[pos(rng)], v[pos(rng)]);
    } else if (d == "heavytail" || d == "sentinel") {
        const bool sentinel = d == "sentinel";
        std::bernoulli_distribution rare(sentinel ? 0.02 : 0.01);
        std::uniform_int_distribution<uint64_t> small(0, 1ull << (sentinel ? 24 : 20));
        for (T &value : v) {
            if (rare(rng)) value = sentinel ? std::numeric_limits<T>::max() : to_key<T>(unif(rng) * (std::is_floating_point_v<T> ? 1e19 : span));
            else value = static_cast<T>(small(rng));
        }
    } else if (d == "span1e19" || d == "span1p3e12") {
        double width = d == "span1e19" ? 1e19 : 1.3e12;
        if constexpr (std::is_integral_v<T>) width *= span / 1.8e19;
        fill([&] { return unif(rng) * width; });
    } else if (d == "signednormal") {
        std::normal_distribution<double> normal(0.0, 1.0);
        if constexpr (std::is_floating_point_v<T>) fill([&] { return normal(rng); });
        else fill([&] { return (normal(rng) * 0.1 + (std::is_unsigned_v<T> ? 0.5 : 0.0)) * span; });
    } else die("unknown --dist " + d);
}

// ---------------------------------------------------------------------------
// Algorithms
// ---------------------------------------------------------------------------

// IPS2Ra sorts by an unsigned key; map signed and floating keys to an order-preserving unsigned image.
template <typename T>
static auto radix_key(T x) {
    if constexpr (std::is_unsigned_v<T>) {
        return x;
    } else if constexpr (std::is_integral_v<T>) {
        using U = std::make_unsigned_t<T>;
        return static_cast<U>(static_cast<U>(x) ^ (U{1} << (8 * sizeof(T) - 1)));
    } else {
        uint64_t bits = std::bit_cast<uint64_t>(x);
        return bits >> 63 ? ~bits : bits | (uint64_t{1} << 63);
    }
}

template <typename T>
static std::map<std::string, std::function<void(std::vector<T> &)>> algorithms() {
    using It = typename std::vector<T>::iterator;
    return {
        {"classic5", [](auto &v) { nfs_r5::sort(v.begin(), v.end()); }},
        {"lowmem2", [](auto &v) { nfs_radix_lowmem2::sort(v.begin(), v.end()); }},
        {"nfss", [](auto &v) { nfs::sort(v.begin(), v.end()); }},
        {"nfss_base", [](auto &v) { nfs_base::sort(v.begin(), v.end()); }},
#ifdef NFS_BENCH_RADIX
        {"radix", [](auto &v) { nfs_radix::sort(v.begin(), v.end()); }},
#endif
#ifdef NFS_BENCH_CLASSIC
        {"classic", [](auto &v) { nfs_classic::sort(v.begin(), v.end()); }},
#endif
        {"std", [](auto &v) { std::sort(v.begin(), v.end()); }},
        {"pdq", [](auto &v) { pdqsort(v.begin(), v.end()); }},
        {"ips4o", [](auto &v) { ips4o::sort(v.begin(), v.end()); }},
        {"ips2ra", [](auto &v) { ips2ra::sort(v.begin(), v.end(), [](T x) { return radix_key(x); }); }},
        {"spread", [](auto &v) { boost::sort::spreadsort::spreadsort(v.begin(), v.end()); }},
        {"bls", [](auto &v) {
             ls_framework::sort<indexes::BalancedMinMaxIndex<It>, indexes::MinMaxIndex<It>, 600>(v.begin(), v.end());
         }},
    };
}

// Order-independent fingerprint of the multiset, so validation does not need a reference sort.
template <typename T>
static uint64_t fingerprint(const std::vector<T> &v) {
    uint64_t sum = 0, mix = 0;
    for (const T &x : v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &x, sizeof(T));
        uint64_t h = (bits + 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull;
        h ^= h >> 31;
        sum += h;
        mix ^= h * 0x94d049bb133111ebull;
    }
    return sum ^ std::rotl(mix, 17);
}

template <typename T>
static int run(const Options &o, SosdPool *pool) {
    auto all = algorithms<T>();
    std::vector<std::string> names = split(o.algos, ',');
    for (auto &a : names)
        if (!all.count(a)) die("unknown algo " + a);

    std::mt19937_64 rng(o.seed);
    std::vector<T> input, work;
    std::map<std::string, std::vector<double>> times;
    int failures = 0;

    generate<T>(o, input, rng, pool);
    const uint64_t fp = fingerprint(input);
    if (o.check_only) {
        auto less = [](T a, T b) {
            if constexpr (std::is_floating_point_v<T>) return !std::isnan(a) && (std::isnan(b) || a < b);
            else return a < b;
        };
        using Bits = std::conditional_t<sizeof(T) == 8, uint64_t, uint32_t>;
        const auto bit_less = [](T a, T b) { return std::bit_cast<Bits>(a) < std::bit_cast<Bits>(b); };
        const auto bit_equal = [](T a, T b) { return std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b); };
        const bool edge = o.dist.starts_with("edge_");
        auto reference = edge ? input : std::vector<T>{};
        if (edge) std::sort(reference.begin(), reference.end(), bit_less);
        for (const auto &name : names) {
            work = input;
            all.at(name)(work);
            bool valid = std::is_sorted(work.begin(), work.end(), less) && fingerprint(work) == fp;
            if (edge && valid) {
                std::sort(work.begin(), work.end(), bit_less);
                valid = std::equal(work.begin(), work.end(), reference.begin(), bit_equal);
            }
            if (!valid) {
                std::cerr << "FAIL " << o.dist << ',' << o.type << ',' << o.n << ',' << name << '\n';
                ++failures;
            }
        }
        std::cout << (failures ? "FAIL " : "PASS ") << o.dist << ',' << o.type << ',' << o.n << ',' << fp << '\n';
        return failures ? 1 : 0;
    }
    for (int r = 0; r < o.runs; ++r) {
        // Rotate the order so no algorithm always runs first on a cold cache.
        for (size_t k = 0; k < names.size(); ++k) {
            const std::string &a = names[(k + r) % names.size()];
            work = input;
            auto t0 = Clock::now();
            all[a](work);
            auto t1 = Clock::now();
            double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
            bool ok = std::is_sorted(work.begin(), work.end()) && fingerprint(work) == fp;
            if (!ok) {
                std::cerr << "FAIL " << a << " run " << r << " produced wrong output\n";
                return 1;
            }
            std::printf("%s,%s,%zu,%s,%d,%.0f\n", o.dist.c_str(), o.type.c_str(), o.n, a.c_str(), r, ns);
            times[a].push_back(ns);
        }
    }

    std::fflush(stdout);
    std::fprintf(stderr, "%s %s n=%zu runs=%d (median ns/key, speedup of nfss = other/nfss)\n",
                 o.dist.c_str(), o.type.c_str(), o.n, o.runs);
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return (v[(v.size() - 1) / 2] + v[v.size() / 2]) / 2;
    };
    double nfss_med = times.count("nfss") ? median(times["nfss"]) : 0;
    for (auto &a : names) {
        double m = median(times[a]);
        std::fprintf(stderr, "  %-7s %8.2f ns/key", a.c_str(), m / o.n);
        if (nfss_med > 0 && a != "nfss") std::fprintf(stderr, "   nfss x%.2f", m / nfss_med);
        std::fprintf(stderr, "\n");
    }
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    Options o;
    std::vector<std::string> sizes{"1000000"}, repetitions{"10"};
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) die("missing value for " + k); return argv[++i]; };
        if (k == "--dist") o.dist = val();
        else if (k == "--type") o.type = val();
        else if (k == "--n") sizes = split(val(), ',');
        else if (k == "--runs") repetitions = split(val(), ',');
        else if (k == "--check") o.check_only = true;
        else if (k == "--seed") o.seed = std::stoull(val());
        else if (k == "--algos") o.algos = val();
        else if (k == "--data") o.data_dir = val();
        else die("unknown flag " + k);
    }
    // macOS cannot pin to a core; the highest QoS keeps the thread on performance cores.
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    if (repetitions.empty() || sizes.empty() || (repetitions.size() != 1 && repetitions.size() != sizes.size()))
        die("--runs needs one count or one count per size");
    std::unique_ptr<SosdPool> pool;
    if (auto it = kSosdFiles.find(o.dist); it != kSosdFiles.end())
        pool = std::make_unique<SosdPool>(o.data_dir + "/" + it->second);
    const auto types = split(o.type, ',');
    if (types.empty()) die("--type is empty");
    int failed = 0;
    for (const auto &type : types) {
        o.type = type;
        for (size_t i = 0; i < sizes.size(); ++i) {
            o.n = static_cast<size_t>(std::stod(sizes[i]));
            o.runs = std::stoi(repetitions[repetitions.size() == 1 ? 0 : i]);
            o.last_input = i + 1 == sizes.size() && &type == &types.back();
            if (!o.n || o.runs < 1) die("sizes and run counts must be positive");
            if (type == "u64") failed |= run<uint64_t>(o, pool.get());
            else if (type == "i64") failed |= run<int64_t>(o, pool.get());
            else if (type == "u32") failed |= run<uint32_t>(o, pool.get());
            else if (type == "f64") failed |= run<double>(o, pool.get());
            else die("unknown --type " + type);
        }
    }
    return failed;
}
