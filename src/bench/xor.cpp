// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <bench/data/xor_histogram.raw.h>
#include <random.h>
#include <span.h>
#include <streams.h>

#include <cmath>
#include <cstddef>
#include <map>
#include <vector>

std::pair<int32_t, int64_t> read_deltas(const std::byte* p)
{
    const uint64_t packed = ReadLE64(reinterpret_cast<const unsigned char*>(p));

    // Extract size (24 bits) and sign extend if negative
    int32_t size = packed & 0xFFFFFF;
    if (size & 0x800000) size |= 0xFF000000;

    // Extract count (40 bits) and sign extend if negative
    int64_t count = packed >> 24;
    if (count & 0x8000000000) count |= 0xFFFFFF0000000000;

    return {size, count};
}

static void XorHistogram(benchmark::Bench& bench)
{
    // The histogram represents util::Xor method's write.size() histograms for the first 860k blocks
    // aggregated and encoded with https://gist.github.com/l0rinc/a44da845ad32ec89c30525507cdd28ee
    std::vector<std::pair<uint64_t, uint64_t>> histogram;

    int64_t prev_size{0};
    int64_t prev_count{0};
    int64_t max_count{0};

    // Read 8-byte chunks containing both deltas
    using namespace benchmark::data;
    for (size_t i{0}; i < xor_histogram.size(); i += 8) {
        const auto [size_delta, count_delta] = read_deltas(xor_histogram.data() + i);
        prev_size += size_delta;
        prev_count += count_delta;

        histogram.emplace_back(prev_size, prev_count);
        max_count = std::max(max_count, prev_count);
    }
    assert(max_count == 47'584'838'861); // 1 byte vectors are most frequent
    constexpr auto scaling_factor{1'000'000U}; // Scale to this many occurrences of 1 byte vectors

    FastRandomContext rng{/*fDeterministic=*/true};
    const uint64_t max_pattern_size{histogram.back().first};
    assert(max_pattern_size == 3'992'470); // Largest vector in histogram
    const auto pattern{rng.randbytes<std::byte>(max_pattern_size)}; // Reuse random pattern since randbytes is slow

    std::vector<std::vector<std::byte>> test_data;
    test_data.reserve(2'036'522);
    uint64_t total_bytes{0};

    for (const auto& [size, count] : histogram) {
        const size_t scaled_count{static_cast<size_t>(std::ceil(static_cast<double>(count) / max_count * scaling_factor))};

        total_bytes += scaled_count * size;
        for (size_t j{0}; j < scaled_count; ++j) {
            test_data.emplace_back(pattern.begin(), pattern.begin() + size);
        }
    }
    assert(total_bytes == 8'129'394'848); // ~8 GB of data
    std::ranges::shuffle(test_data, rng); // Make it more realistic & less predictable

    std::vector key_bytes{rng.randbytes<std::byte>(8)};
    uint64_t key;
    std::memcpy(&key, key_bytes.data(), 8);

    size_t offset{0};
    bench.batch(total_bytes).unit("byte").run([&] {
        for (auto& data : test_data) {
            util::Xor(data, key, offset++);
        }
        ankerl::nanobench::doNotOptimizeAway(test_data);
    });
}

static void AutoFileXor(benchmark::Bench& bench)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    const auto data{rng.randbytes<std::byte>(1'000'000)};

    std::vector<std::byte> empty_key_bytes(8, std::byte{0}); // Test disabled xor
    uint64_t empty_key;
    std::memcpy(&empty_key, empty_key_bytes.data(), 8);

    const fs::path test_path = fs::temp_directory_path() / "xor_benchmark.dat";
    AutoFile f{fsbridge::fopen(test_path, "wb+"), empty_key_bytes};
    bench.batch(data.size()).unit("byte").run([&] {
        f.Truncate(0);
        f << data;
    });
    try { fs::remove(test_path); } catch (const fs::filesystem_error&) {
    }
}

BENCHMARK(XorHistogram, benchmark::PriorityLevel::LOW);
BENCHMARK(AutoFileXor, benchmark::PriorityLevel::LOW);