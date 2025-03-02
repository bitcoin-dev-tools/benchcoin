// Copyright (c) 2016-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data/block413567.raw.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/validation.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <span.h>
#include <stdexcept>
#include <streams.h>
#include <string>
#include <util/chaintype.h>
#include <validation.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <random.h>
#include <vector>

// These are the two major time-sinks which happen after we have fully received
// a block off the wire, but before we can relay the block on to peers using
// compact block relay.

static void DeserializeBlockTest(benchmark::Bench& bench)
{
    DataStream stream(benchmark::data::block413567);
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction

    bench.unit("block").run([&] {
        CBlock block;
        stream >> TX_WITH_WITNESS(block);
        bool rewound = stream.Rewind(benchmark::data::block413567.size());
        assert(rewound);
    });
}

static void DeserializeAndCheckBlockTest(benchmark::Bench& bench)
{
    DataStream stream(benchmark::data::block413567);
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction

    ArgsManager bench_args;
    const auto chainParams = CreateChainParams(bench_args, ChainType::MAIN);

    bench.unit("block").run([&] {
        CBlock block; // Note that CBlock caches its checked state, so we need to recreate it here
        stream >> TX_WITH_WITNESS(block);
        bool rewound = stream.Rewind(benchmark::data::block413567.size());
        assert(rewound);

        BlockValidationState validationState;
        bool checked = CheckBlock(block, validationState, chainParams->GetConsensus());
        assert(checked);
    });
}

static std::vector<COutPoint> GetOutpoints()
{
    CBlock block;
    DataStream(benchmark::data::block413567) >> TX_WITH_WITNESS(block);

    std::vector<COutPoint> outpoints;
    for (const auto& tx : block.vtx) {
        for (const auto& in : tx->vin) {
            outpoints.emplace_back(in.prevout);
        }
    }
    outpoints.shrink_to_fit();
    std::ranges::shuffle(outpoints, FastRandomContext(/*fDeterministic=*/false));
    return outpoints;
}

namespace {
struct BenchCoinEntry {
    COutPoint* outpoint;
    uint8_t key;
    explicit BenchCoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)), key(DB_COIN)  {}

    SERIALIZE_METHODS(BenchCoinEntry, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};
}

static void SerializeCOutPoint(benchmark::Bench& bench)
{
    const auto& ops{GetOutpoints()};

    std::vector<BenchCoinEntry> bench_coin_entries;
    bench_coin_entries.reserve(ops.size());
    for (auto& op : ops) bench_coin_entries.emplace_back(&op);

    DataStream original;
    for (auto& op : bench_coin_entries) original << op;

    bench.warmup(1).batch(ops.size()).unit("outpoints").run([&] {
        DataStream serialized;
        for (auto& op : bench_coin_entries) serialized << op;
        assert(serialized.size() == original.size());
        // assert(serialized.str() == original.str());
    });
}

static void SerializeCOutPoint2(benchmark::Bench& bench)
{
    const auto& ops{GetOutpoints()};

    DataStream original;
    for (auto& op : ops) original << BenchCoinEntry{&op};

    DataStream serialized;
    serialized.resize(original.size());
    bench.warmup(1).batch(ops.size()).unit("outpoints").run([&] {
        serialized.clear();
        for (auto& op : ops) WriteCOutPoint(serialized, op);
        assert(serialized.size() == original.size());
        assert(serialized.str() == original.str());
    });
}

static void DeserializeCOutPoint(benchmark::Bench& bench)
{
    const auto& ops{GetOutpoints()};

    DataStream serialized;
    for (auto& op : ops) serialized << op;

    COutPoint outpoint;
    bench.batch(ops.size()).unit("outpoints").run([&] {
        serialized.clear();
        for (auto& op : ops) {
            serialized >> BenchCoinEntry{&outpoint};
            assert(op == outpoint);
        }
    });
}

static void DeserializeCOutPoint2(benchmark::Bench& bench)
{
    const auto& outpoints{GetOutpoints()};

    std::vector<DataStream> serialized_outpoints;
    DataStream key_buffer;
    key_buffer.resize(MAX_COUTPOINT_SERIALIZED_SIZE);
    for (auto op : outpoints) {
        WriteCOutPoint(key_buffer, op);
        serialized_outpoints.emplace_back(key_buffer);
    }

    DataStream stream;
    for (auto& op : outpoints) stream << op;
    COutPoint outpoint;
    bench.batch(outpoints.size()).unit("outpoints").run([&] {
        for (auto& op : outpoints) {
            ReadCOutPoint(stream, outpoint);
            assert(op == outpoint);
        }
    });
}

BENCHMARK(DeserializeBlockTest, benchmark::PriorityLevel::HIGH);
BENCHMARK(DeserializeAndCheckBlockTest, benchmark::PriorityLevel::HIGH);
BENCHMARK(SerializeCOutPoint, benchmark::PriorityLevel::LOW);
BENCHMARK(SerializeCOutPoint2, benchmark::PriorityLevel::LOW);
BENCHMARK(DeserializeCOutPoint, benchmark::PriorityLevel::LOW);
BENCHMARK(DeserializeCOutPoint2, benchmark::PriorityLevel::LOW);
