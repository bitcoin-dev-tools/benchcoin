// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests of block validation that need no TestingSetup.

#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

BOOST_AUTO_TEST_SUITE(validation_core_tests)

//! The script execution cache is consulted without cs_main while entries are inserted (ValidationCache's mutex):
//! lookups beside inserts, with and without erasing, never find an entry that was never inserted.
BOOST_AUTO_TEST_CASE(script_execution_cache_lookups_beside_inserts)
{
    ValidationCache cache{/*script_execution_cache_bytes=*/1 << 20, /*signature_cache_bytes=*/1 << 20};
    const auto key{[](uint8_t tag, uint32_t i) {
        uint256 k;
        k.data()[0] = tag;
        for (int b{0}; b < 4; ++b) k.data()[1 + b] = static_cast<uint8_t>(i >> (8 * b));
        return k;
    }};
    constexpr uint32_t COUNT{20000};
    std::atomic<bool> false_hit{false};
    std::thread inserter{[&] {
        for (uint32_t i{0}; i < COUNT; ++i) cache.CacheScriptExecution(key(1, i));
    }};
    std::vector<std::thread> readers;
    for (const bool erase : {false, true}) {
        readers.emplace_back([&, erase] {
            for (int round{0}; round < 4; ++round) {
                for (uint32_t i{0}; i < COUNT; ++i) {
                    (void)cache.ScriptExecutionCached(key(1, i), erase);
                    if (cache.ScriptExecutionCached(key(2, i), erase)) false_hit = true;
                }
            }
        });
    }
    inserter.join();
    for (std::thread& reader : readers) reader.join();
    BOOST_CHECK(!false_hit);
}

BOOST_AUTO_TEST_SUITE_END()
