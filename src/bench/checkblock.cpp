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
    return outpoints;
}

static void SerializeCOutPoint(benchmark::Bench& bench)
{
    std::vector<CoinEntry> outpoints;
    for (auto out : GetOutpoints()) {
        outpoints.emplace_back(&out);
    }

    DataStream original;
    for (auto& op : outpoints) original << op;

    bench.warmup(1).batch(outpoints.size()).unit("outpoints").run([&] {
        DataStream serialized;
        serialized.reserve(original.size());
        for (auto& op : outpoints) serialized << op;
        assert(serialized.size() == original.size());
    });
}

static void SerializeCOutPoint2(benchmark::Bench& bench)
{
    const auto& outpoints{GetOutpoints()};

    DataStream original;
    for (auto& outpoint : outpoints) original << CoinEntry{&outpoint};

    bench.warmup(1).batch(outpoints.size()).unit("outpoints").run([&] {
        std::string serialized;
        serialized.reserve(original.size());
        for (auto& op : outpoints) WriteCOutPoint(serialized, op);
        assert(serialized.size() == original.size());
    });
}

BENCHMARK(SerializeCOutPoint, benchmark::PriorityLevel::HIGH);
BENCHMARK(SerializeCOutPoint2, benchmark::PriorityLevel::HIGH);
BENCHMARK(DeserializeBlockTest, benchmark::PriorityLevel::HIGH);
BENCHMARK(DeserializeAndCheckBlockTest, benchmark::PriorityLevel::HIGH);