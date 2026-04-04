// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <util/time.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
class CBlockHeader
{
public:
    // header
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    CBlockHeader()
    {
        SetNull();
    }

    SERIALIZE_METHODS(CBlockHeader, obj) { READWRITE(obj.nVersion, obj.hashPrevBlock, obj.hashMerkleRoot, obj.nTime, obj.nBits, obj.nNonce); }

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        nNonce = 0;
    }

    bool IsNull() const
    {
        return (nBits == 0);
    }

    uint256 GetHash() const;

    NodeSeconds Time() const
    {
        return NodeSeconds{std::chrono::seconds{nTime}};
    }

    int64_t GetBlockTime() const
    {
        return (int64_t)nTime;
    }
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;

    // Memory-only flags for caching expensive checks (atomic for thread safety)
    mutable std::atomic<bool> fChecked{false};                    // CheckBlock()
    mutable std::atomic<bool> m_checked_witness_commitment{false}; // CheckWitnessCommitment()
    mutable std::atomic<bool> m_checked_merkle_root{false};        // CheckMerkleRoot()

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    CBlock(const CBlock& other) : CBlockHeader(other), vtx(other.vtx),
        fChecked(other.fChecked.load(std::memory_order_relaxed)),
        m_checked_witness_commitment(other.m_checked_witness_commitment.load(std::memory_order_relaxed)),
        m_checked_merkle_root(other.m_checked_merkle_root.load(std::memory_order_relaxed)) {}

    CBlock(CBlock&& other) noexcept : CBlockHeader(std::move(other)), vtx(std::move(other.vtx)),
        fChecked(other.fChecked.load(std::memory_order_relaxed)),
        m_checked_witness_commitment(other.m_checked_witness_commitment.load(std::memory_order_relaxed)),
        m_checked_merkle_root(other.m_checked_merkle_root.load(std::memory_order_relaxed)) {}

    CBlock& operator=(const CBlock& other)
    {
        if (this != &other) {
            CBlockHeader::operator=(other);
            vtx = other.vtx;
            fChecked.store(other.fChecked.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_checked_witness_commitment.store(other.m_checked_witness_commitment.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_checked_merkle_root.store(other.m_checked_merkle_root.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    CBlock& operator=(CBlock&& other) noexcept
    {
        if (this != &other) {
            CBlockHeader::operator=(std::move(other));
            vtx = std::move(other.vtx);
            fChecked.store(other.fChecked.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_checked_witness_commitment.store(other.m_checked_witness_commitment.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_checked_merkle_root.store(other.m_checked_merkle_root.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITE(AsBase<CBlockHeader>(obj), obj.vtx);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked.store(false, std::memory_order_relaxed);
        m_checked_witness_commitment.store(false, std::memory_order_relaxed);
        m_checked_merkle_root.store(false, std::memory_order_relaxed);
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    /** Historically CBlockLocator's version field has been written to network
     * streams as the negotiated protocol version and to disk streams as the
     * client version, but the value has never been used.
     *
     * Hard-code to the highest protocol version ever written to a network stream.
     * SerParams can be used if the field requires any meaning in the future,
     **/
    static constexpr int DUMMY_VERSION = 70016;

    std::vector<uint256> vHave;

    CBlockLocator() = default;

    explicit CBlockLocator(std::vector<uint256>&& have) : vHave(std::move(have)) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        int nVersion = DUMMY_VERSION;
        READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
