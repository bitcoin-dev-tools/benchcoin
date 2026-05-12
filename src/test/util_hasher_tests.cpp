// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/common.h>
#include <primitives/transaction_identifier.h>
#include <uint256.h>
#include <util/hasher.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <ranges>

BOOST_AUTO_TEST_SUITE(util_hasher_tests)

namespace {
uint256 MakeUint256(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint256 u{};
    WriteLE64(u.begin(),      a);
    WriteLE64(u.begin() + 8,  b);
    WriteLE64(u.begin() + 16, c);
    WriteLE64(u.begin() + 24, d);
    return u;
}
} // namespace

// With a deterministic (zero) key, QuickHasher reduces to summing the four uint64_t
// chunks of the txid. Useful when callers want reproducible bucketing in tests.
BOOST_AUTO_TEST_CASE(quickhash_deterministic_zero_key)
{
    QuickHasher hasher{/*deterministic=*/true};
    BOOST_CHECK_EQUAL(hasher(Txid::FromUint256(uint256::ZERO)), 0u);

    const uint256 u{MakeUint256(0x0102030405060708ULL, 0x1112131415161718ULL,
                                0x2122232425262728ULL, 0x3132333435363738ULL)};
    const auto h{hasher(Txid::FromUint256(u))};
    BOOST_CHECK_EQUAL(h, 0x0102030405060708ULL + 0x1112131415161718ULL +
                            0x2122232425262728ULL + 0x3132333435363738ULL);

    // Swapping the lower two uint64_t chunks preserves the sum and therefore collides on
    // the hash; equality checks on the full Txid still distinguish the two values.
    uint256 swapped{u};
    std::swap_ranges(swapped.begin(), swapped.begin() + 8, swapped.begin() + 8);
    BOOST_CHECK_EQUAL(hasher(Txid::FromUint256(swapped)), h);
    BOOST_CHECK(Txid::FromUint256(u) != Txid::FromUint256(swapped));
}

// Wrapping addition is intentional and well-defined for unsigned integers.
BOOST_AUTO_TEST_CASE(quickhash_wraps_on_overflow)
{
    QuickHasher hasher{/*deterministic=*/true};
    const uint256 u{MakeUint256(~uint64_t{0}, ~uint64_t{0}, ~uint64_t{0}, ~uint64_t{0})};
    // Four * (2^64 - 1) wraps modulo 2^64 to (2^64 - 4), i.e. ~uint64_t{0} - 3.
    BOOST_CHECK_EQUAL(hasher(Txid::FromUint256(u)), ~uint64_t{0} - 3);
}

// A non-deterministic hasher distinguishes inputs whose chunk-sums collide under the
// deterministic key, so swap_ranges should produce a different hash with overwhelming
// probability.
BOOST_AUTO_TEST_CASE(quickhash_random_key_distinguishes_swaps)
{
    QuickHasher hasher{/*deterministic=*/false};
    const uint256 u{MakeUint256(0xAABBCCDDEEFF0011ULL, 0x1122334455667788ULL,
                                0xDEADBEEFCAFEBABEULL, 0xFEEDFACE0DDF00D5ULL)};
    uint256 swapped{u};
    std::swap_ranges(swapped.begin(), swapped.begin() + 8, swapped.begin() + 8);
    BOOST_CHECK_NE(hasher(Txid::FromUint256(u)), hasher(Txid::FromUint256(swapped)));
}

BOOST_AUTO_TEST_SUITE_END()
