// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <attributes.h>
#include <bench/bench.h>
#include <coins.h>
#include <crypto/siphash.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <random.h>
#include <uint256.h>
#include <util/hasher.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <vector>

// Microbenchmarks isolating the per-input/per-tx loop in
// CoinsViewOverlay::StartFetching. All variants pre-reserve the inputs vector
// and a per-block txid set; the difference is the txid set's key/hasher and
// whether the inline self-spend filter is performed at populate time.
//
//   A) PreReservedSet - current approach. The set stores 64-bit QuickHashes
//      of txids and is consulted later by worker threads, so populate just
//      pushes every input and emplaces every txid hash.
//   E) HoistedSet - same as A) but additionally performs the inline filter
//      (skip pushing inputs whose prevout's QuickHash already appears in the
//      set). Models doing the same filtering work that A's workers do, but at
//      populate time.
//   F) HoistedSetTxid - inline filter, with the txid set keyed by Txid (32B)
//      and hashed by SaltedTxidHasher (SipHash-2-4 over uint256).
//   G) HoistedSetTxidQuickHash - inline filter, txid set keyed by Txid but
//      hashed by QuickHashHasher (xor-add of 4 uint64 limbs against a salt).
//      Isolates the cost of the wider key from the hasher choice.
//   J) HoistedSetTxidJumbo - inline filter, txid set keyed by Txid hashed by
//      a SipHash-1-3 jumboblock variant adapted from
//      https://github.com/l0rinc/bitcoin/pull/70. The full 256-bit hash is
//      mixed in a single SipRound (instead of four message blocks), so the
//      cost is one compression round + three finalization rounds = 4 rounds
//      total per call (vs 14 for SaltedTxidHasher's SH24+UP path on outpoints).
//
// All benchmarks build the same block once (deterministically) so the only
// difference between them is the populate strategy.
namespace {

constexpr size_t TX_COUNT{5'000};
constexpr size_t INPUTS_PER_TX{4};
constexpr size_t TOTAL_INPUTS{TX_COUNT * INPUTS_PER_TX};

using QuickHash = uint64_t;

//! Mirrors CoinsViewOverlay::QuickHashHasher. Reproduced here so the benchmark
//! does not depend on internals of CoinsViewOverlay.
class QuickHashHasher
{
    uint64_t m_key[4];

public:
    explicit QuickHashHasher(bool deterministic) noexcept
    {
        FastRandomContext rng{deterministic};
        for (auto& k : m_key) k = rng.rand64();
    }

#if defined(__clang__)
    __attribute__((no_sanitize("unsigned-integer-overflow")))
#endif
    QuickHash operator()(const Txid& txid) const noexcept
    {
        const auto& hash_input{txid.ToUint256()};
        QuickHash out{0};
        for (const auto i : std::views::iota(0, 4)) out += hash_input.GetUint64(i) ^ m_key[i];
        return out;
    }
};

//! SipHash-1-3 jumboblock hasher adapted from
//! https://github.com/l0rinc/bitcoin/pull/70 for the Txid-only (32-byte) case.
//! The full uint256 is absorbed as a single block (1 compression SipRound),
//! followed by 3 finalization SipRounds. Replicated locally so the benchmark
//! works on a master tree without merging the PR.
class JumboTxidHasher
{
    uint64_t m_k0;
    uint64_t m_k1;

    ALWAYS_INLINE static void SipRound(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) noexcept
    {
        uint64_t a{v0}, b{v1}, c{v2}, d{v3};
        a += b; b = std::rotl(b, 13); b ^= a;
        a = std::rotl(a, 32);
        c += d; d = std::rotl(d, 16); d ^= c;
        a += d; d = std::rotl(d, 21); d ^= a;
        c += b; b = std::rotl(b, 17); b ^= c;
        c = std::rotl(c, 32);
        v0 = a; v1 = b; v2 = c; v3 = d;
    }

public:
    explicit JumboTxidHasher(bool deterministic) noexcept
    {
        FastRandomContext rng{deterministic};
        m_k0 = rng.rand64();
        m_k1 = rng.rand64();
    }

    ALWAYS_INLINE size_t operator()(const Txid& txid) const noexcept
    {
        static constexpr uint64_t C0{0x736f6d6570736575ULL};
        static constexpr uint64_t C1{0x646f72616e646f6dULL};
        static constexpr uint64_t C2{0x6c7967656e657261ULL};
        static constexpr uint64_t C3{0x7465646279746573ULL};

        const auto& u{txid.ToUint256()};
        const uint64_t m0{u.GetUint64(0)};
        const uint64_t m1{u.GetUint64(1)};
        const uint64_t m2{u.GetUint64(2)};
        const uint64_t m3{u.GetUint64(3)};

        uint64_t v0{C0 ^ m_k0};
        uint64_t v1{C1 ^ m_k1};
        uint64_t v2{C2 ^ m_k0};
        uint64_t v3{C3 ^ m_k1};

        v0 ^= m0;
        v1 ^= m1;
        v2 ^= m2;
        v3 ^= m3;
        SipRound(v0, v1, v2, v3);
        v0 ^= m3;
        v1 ^= m0;
        v2 ^= m1;
        v3 ^= m2;

        v2 ^= 0xff;
        SipRound(v0, v1, v2, v3);
        SipRound(v0, v1, v2, v3);
        SipRound(v0, v1, v2, v3);
        return v0 ^ v1 ^ v2 ^ v3;
    }
};

//! Mirrors CoinsViewOverlay::InputToFetch so the per-input emplace cost is
//! representative.
struct InputToFetch {
    std::atomic_flag ready{};
    const COutPoint& outpoint;
    std::optional<Coin> coin{std::nullopt};

    InputToFetch(InputToFetch&& other) noexcept : outpoint{other.outpoint} {}
    explicit InputToFetch(const COutPoint& o LIFETIMEBOUND) noexcept : outpoint{o} {}
};

//! Build a block with TX_COUNT transactions of INPUTS_PER_TX random prevouts
//! each, plus a coinbase. Prevouts are fully random so the inline filter
//! never short-circuits, exercising the worst case for the variants that pay
//! a per-input lookup.
CBlock CreateRandomBlock()
{
    FastRandomContext rng{/*deterministic=*/true};

    CBlock block;
    block.vtx.reserve(TX_COUNT + 1);

    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.resize(1);
    block.vtx.emplace_back(MakeTransactionRef(std::move(coinbase)));

    for (size_t i{0}; i < TX_COUNT; ++i) {
        CMutableTransaction tx;
        tx.vin.reserve(INPUTS_PER_TX);
        for (size_t j{0}; j < INPUTS_PER_TX; ++j) {
            tx.vin.emplace_back(COutPoint{Txid::FromUint256(rng.rand256()), rng.rand32()});
        }
        tx.vout.resize(1);
        block.vtx.emplace_back(MakeTransactionRef(std::move(tx)));
    }
    return block;
}

//! Approach A: pre-reserved inputs vector and pre-reserved QuickHash txids
//! set, both treated as long-lived members. No inline filter; just push every
//! input and emplace every txid quickhash, the way StartFetching does today.
void CoinsViewOverlayStartFetchingPreReservedSet(benchmark::Bench& bench)
{
    const CBlock block{CreateRandomBlock()};
    const QuickHashHasher hasher{/*deterministic=*/true};

    std::vector<InputToFetch> inputs;
    std::unordered_set<QuickHash> txids;
    inputs.reserve(TOTAL_INPUTS);
    txids.reserve(TX_COUNT);

    bench.unit("block").epochIterations(1)
        .setup([&] {
            inputs.clear();
            txids.clear();
        })
        .run([&] {
            for (const auto& tx : block.vtx | std::views::drop(1)) {
                for (const auto& in : tx->vin) {
                    inputs.emplace_back(in.prevout);
                }
                txids.emplace(hasher(tx->GetHash()));
            }
            ankerl::nanobench::doNotOptimizeAway(inputs);
            ankerl::nanobench::doNotOptimizeAway(txids);
        });
}

//! Approach E: same hoisted QuickHash txids set as A) but adds the inline
//! filter (skip pushing inputs whose prevout-quickhash is already in the set).
void CoinsViewOverlayStartFetchingHoistedSet(benchmark::Bench& bench)
{
    const CBlock block{CreateRandomBlock()};
    const QuickHashHasher hasher{/*deterministic=*/true};

    std::vector<InputToFetch> inputs;
    std::unordered_set<QuickHash> txids;
    inputs.reserve(TOTAL_INPUTS);
    txids.reserve(TX_COUNT);

    bench.unit("block").epochIterations(1)
        .setup([&] {
            inputs.clear();
            txids.clear();
        })
        .run([&] {
            for (const auto& tx : block.vtx | std::views::drop(1)) {
                for (const auto& in : tx->vin) {
                    if (txids.contains(hasher(in.prevout.hash))) continue;
                    inputs.emplace_back(in.prevout);
                }
                txids.emplace(hasher(tx->GetHash()));
            }
            ankerl::nanobench::doNotOptimizeAway(inputs);
            ankerl::nanobench::doNotOptimizeAway(txids);
        });
}

//! Approach F: hoisted Txid+SaltedTxidHasher set with inline filter. Stores
//! 32-byte Txid keys hashed by SipHash-2-4 (SH24+UP path).
void CoinsViewOverlayStartFetchingHoistedSetTxid(benchmark::Bench& bench)
{
    const CBlock block{CreateRandomBlock()};

    std::vector<InputToFetch> inputs;
    std::unordered_set<Txid, SaltedTxidHasher> txids;
    inputs.reserve(TOTAL_INPUTS);
    txids.reserve(TX_COUNT);

    bench.unit("block").epochIterations(1)
        .setup([&] {
            inputs.clear();
            txids.clear();
        })
        .run([&] {
            for (const auto& tx : block.vtx | std::views::drop(1)) {
                for (const auto& in : tx->vin) {
                    if (txids.contains(in.prevout.hash)) continue;
                    inputs.emplace_back(in.prevout);
                }
                txids.emplace(tx->GetHash());
            }
            ankerl::nanobench::doNotOptimizeAway(inputs);
            ankerl::nanobench::doNotOptimizeAway(txids);
        });
}

//! Approach G: hoisted Txid+QuickHashHasher set with inline filter. Same
//! 32-byte key as F) but with the cheap xor-add hasher used in A) and E).
void CoinsViewOverlayStartFetchingHoistedSetTxidQuickHash(benchmark::Bench& bench)
{
    const CBlock block{CreateRandomBlock()};

    std::vector<InputToFetch> inputs;
    std::unordered_set<Txid, QuickHashHasher> txids{
        /*bucket_count=*/0, QuickHashHasher{/*deterministic=*/true}};
    inputs.reserve(TOTAL_INPUTS);
    txids.reserve(TX_COUNT);

    bench.unit("block").epochIterations(1)
        .setup([&] {
            inputs.clear();
            txids.clear();
        })
        .run([&] {
            for (const auto& tx : block.vtx | std::views::drop(1)) {
                for (const auto& in : tx->vin) {
                    if (txids.contains(in.prevout.hash)) continue;
                    inputs.emplace_back(in.prevout);
                }
                txids.emplace(tx->GetHash());
            }
            ankerl::nanobench::doNotOptimizeAway(inputs);
            ankerl::nanobench::doNotOptimizeAway(txids);
        });
}

//! Approach J: hoisted Txid+JumboTxidHasher set with inline filter. Same
//! 32-byte key as F) and G), but hashed with the SipHash-1-3 jumboblock
//! variant from https://github.com/l0rinc/bitcoin/pull/70 (4 SipRounds total
//! for the Txid-only case).
void CoinsViewOverlayStartFetchingHoistedSetTxidJumbo(benchmark::Bench& bench)
{
    const CBlock block{CreateRandomBlock()};

    std::vector<InputToFetch> inputs;
    std::unordered_set<Txid, JumboTxidHasher> txids{
        /*bucket_count=*/0, JumboTxidHasher{/*deterministic=*/true}};
    inputs.reserve(TOTAL_INPUTS);
    txids.reserve(TX_COUNT);

    bench.unit("block").epochIterations(1)
        .setup([&] {
            inputs.clear();
            txids.clear();
        })
        .run([&] {
            for (const auto& tx : block.vtx | std::views::drop(1)) {
                for (const auto& in : tx->vin) {
                    if (txids.contains(in.prevout.hash)) continue;
                    inputs.emplace_back(in.prevout);
                }
                txids.emplace(tx->GetHash());
            }
            ankerl::nanobench::doNotOptimizeAway(inputs);
            ankerl::nanobench::doNotOptimizeAway(txids);
        });
}

} // namespace

BENCHMARK(CoinsViewOverlayStartFetchingPreReservedSet);
BENCHMARK(CoinsViewOverlayStartFetchingHoistedSet);
BENCHMARK(CoinsViewOverlayStartFetchingHoistedSetTxid);
BENCHMARK(CoinsViewOverlayStartFetchingHoistedSetTxidQuickHash);
BENCHMARK(CoinsViewOverlayStartFetchingHoistedSetTxidJumbo);
