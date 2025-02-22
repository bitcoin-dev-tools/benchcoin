// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <dbwrapper.h>
#include <kernel/cs_main.h>
#include <sync.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class COutPoint;
class uint256;

//! -dbbatchsize default (bytes)
static const int64_t nDefaultDbBatchSize = 16 << 20;

static constexpr uint8_t DB_COIN{'C'};
static constexpr uint8_t DB_BEST_BLOCK{'B'};
static constexpr uint8_t DB_HEAD_BLOCKS{'H'};

static constexpr size_t SerializedSize(const COutPoint& op) noexcept
{
    return 1 + sizeof(uint256) + GetVarUInt32Size(op.n);
}

inline size_t WriteCOutPoint(Span<std::byte> out, const COutPoint& op) noexcept
{
    const size_t size{SerializedSize(op)};
    assert(out.size() >= size);

    out[0] = std::byte{DB_COIN};
    std::memcpy(&out[1], op.hash.begin(), sizeof(uint256));
    WriteVarUInt32(out.subspan(1 + sizeof(uint256)), op.n);

    return size;
}

inline void ReadCOutPoint(Span<const std::byte> in, COutPoint& op)
{
    assert(!in.empty());
    assert(static_cast<uint8_t>(in[0]) == DB_COIN);
    in = in.subspan(1);

    assert(in.size() >= sizeof(uint256));
    op.hash = Txid::FromUint256(uint256({reinterpret_cast<const uint8_t*>(in.begin()), sizeof(uint256)}));

    ReadVarUInt32(in.subspan(sizeof(uint256)), op.n);
}

//! User-controlled performance and debug options.
struct CoinsViewOptions {
    //! Maximum database write batch size in bytes.
    size_t batch_write_bytes = nDefaultDbBatchSize;
    //! If non-zero, randomly exit when the database is flushed with (1/ratio)
    //! probability.
    int simulate_crash_ratio = 0;
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView
{
protected:
    DBParams m_db_params;
    CoinsViewOptions m_options;
    std::unique_ptr<CDBWrapper> m_db;
public:
    explicit CCoinsViewDB(DBParams db_params, CoinsViewOptions options);

    std::optional<Coin> GetCoin(const COutPoint& outpoint, Span<std::byte> key_buffer) const override;
    bool HaveCoin(const COutPoint &outpoint, Span<std::byte> key_buffer) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    bool BatchWrite(CoinsViewCacheCursor& cursor, const uint256 &hashBlock) override;
    std::unique_ptr<CCoinsViewCursor> Cursor() const override;

    //! Whether an unsupported database format is used.
    bool NeedsUpgrade();
    size_t EstimateSize() const override;

    //! Dynamically alter the underlying leveldb cache size.
    void ResizeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! @returns filesystem path to on-disk storage or std::nullopt if in memory.
    std::optional<fs::path> StoragePath() { return m_db->StoragePath(); }
};

#endif // BITCOIN_TXDB_H
