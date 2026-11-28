// Fran-style f64 benchmark: rotating algorithms, 1M doubles in [-1e9, 1e9].
#include "../nfss.h"
#include "../nfss_r5.h"
#include "../nfss_radix_lowmem2.h"
#ifdef NFS_BENCH_RADIX
#include "nfs_radix.h"
#endif
#include "nfss_base.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using V = std::vector<double>;
static V gen(const std::string &d, size_t n, std::mt19937_64 &r) {
    V v(n);
    std::uniform_real_distribution<double> u(-1e9, 1e9);
    auto clip = [](double x) { return std::clamp(x, -1e9, 1e9); };
    if (d == "uniform" || d == "sorted" || d == "reverse" || d == "nearlysorted" || d == "organpipe") for (auto &x : v) x = u(r);
    if (d == "normal") { std::normal_distribution<double> g(0, 1e6); for (auto &x : v) x = clip(g(r)); }
    if (d == "exponential") { std::exponential_distribution<double> e(1.5e-9); for (auto &x : v) x = clip(-1e9 + e(r)); }
    if (d == "fewunique") { V k(50); for (auto &x : k) x = u(r); std::uniform_int_distribution<int> i(0, 49); for (auto &x : v) x = k[i(r)]; }
    if (d == "clustered") { V c(4); for (auto &x : c) x = u(r); std::normal_distribution<double> g(0, 20000); std::uniform_int_distribution<int> i(0, 3); for (auto &x : v) x = clip(c[i(r)] + g(r)); }
    if (d == "sorted" || d == "nearlysorted" || d == "organpipe") std::sort(v.begin(), v.end());
    if (d == "reverse") std::sort(v.rbegin(), v.rend());
    if (d == "nearlysorted") { std::uniform_int_distribution<size_t> i(0, n - 1); for (size_t s = 0; s < n / 100; s++) std::swap(v[i(r)], v[i(r)]); }
    if (d == "organpipe") { V t(n); size_t a = 0, b = n - 1; for (size_t i = 0; i < n; i++) (i % 2 ? t[b--] : t[a++]) = v[i]; v = t; }
    return v;
}
int main(int argc, char **argv) {
    size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000; int runs = 15;
    const char *dists[] = {"uniform", "normal", "exponential", "fewunique", "clustered", "sorted", "reverse", "nearlysorted", "organpipe"};
    using Sort = void (*)(V::iterator, V::iterator);
    const Sort sorts[] = {nfs_base::sort, nfs::sort, nfs_radix_lowmem2::sort, nfs_r5::sort,
#ifdef NFS_BENCH_RADIX
                           nfs_radix::sort
#endif
    };
    constexpr unsigned count = std::size(sorts);
    std::printf("distribution,original_ms,classic_ms,lowmem2_ms,classic5_ms");
#ifdef NFS_BENCH_RADIX
    std::printf(",radix_ms");
#endif
    std::puts("");
    for (auto d : dists) {
        std::mt19937_64 rng(42);
        V input = gen(d, n, rng), ref = input;
        std::sort(ref.begin(), ref.end());
        std::vector<double> times[count];
        for (int run = 0; run < runs; ++run) {
            for (unsigned k = 0; k < count; ++k) {
                const auto a = (run + k) % count;
                V work = input;
                const auto start = std::chrono::steady_clock::now();
                sorts[a](work.begin(), work.end());
                const auto elapsed = std::chrono::steady_clock::now() - start;
                times[a].push_back(std::chrono::duration<double, std::milli>(elapsed).count());
                if (work != ref) {
                    std::fprintf(stderr, "%s: wrong output for sort %u\n", d, a);
                    return 1;
                }
            }
        }
        std::printf("%s", d);
        for (auto &t : times) {
            std::sort(t.begin(), t.end());
            std::printf(",%.4f", t[runs / 2]);
        }
        std::puts("");
    }
}
