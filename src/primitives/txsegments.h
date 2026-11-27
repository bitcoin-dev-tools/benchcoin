// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_TXSEGMENTS_H
#define BITCOIN_PRIMITIVES_TXSEGMENTS_H

#include <crypto/sha256.h>
#include <primitives/transaction_identifier.h>
#include <serialize.h>
#include <uint256.h>
#include <util/check.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <ios>
#include <span>
#include <utility>

/**
 * Where one transaction lies in a buffer: the whole transaction (the wtxid preimage) and the segments of it that
 * form the txid preimage, which leave out the segwit marker, flag and witness. Recorded by TxSegmentRecorder while
 * UnserializeTransaction() parses, hashed by HashTransactionSegments(); together they compute a transaction's
 * txid and wtxid from the bytes it was read from instead of by serialising it again.
 *
 * The two agree because every encoding UnserializeTransaction() accepts serialises back to the same bytes: compact
 * sizes must be minimal, unknown flag bits and a witness of only empty stacks are rejected. A decoder that came to
 * accept a looser encoding would make the hash of the bytes read differ from the hash of the transaction, and its
 * transactions would have to be hashed by serialising them (CTransaction::HashBySerialising()).
 */
struct TxSegments {
    size_t begin{0};
    size_t end{0};
    //! At most three: up to the marker, from the flag to the witness, after the witness.
    std::array<std::pair<size_t, size_t>, 3> txid{};
    size_t txid_count{0};
    //! The segwit framing was present. After a successful parse that is the same as CTransaction::HasWitness(),
    //! since a framing with only empty witness stacks is rejected.
    bool has_witness{false};
};

/**
 * A reader over a buffer that records, through the TxidHashingStream calls UnserializeTransaction() makes, which
 * bytes of each transaction form its txid preimage. Nothing is hashed here. Begin() before a transaction, End()
 * after it; Pos() is the offset of the next byte. A read past the end throws std::ios_base::failure, as a
 * SpanReader's does.
 */
class TxSegmentRecorder
{
    std::span<const std::byte> m_rest;
    const size_t m_total;
    TxSegments* m_segments{nullptr};
    size_t m_segment_start{0};
    size_t m_hold_start{0};

    void Close(size_t end)
    {
        Assert(m_segments->txid_count < m_segments->txid.size());
        m_segments->txid[m_segments->txid_count++] = {m_segment_start, end};
    }

public:
    explicit TxSegmentRecorder(std::span<const std::byte> data) : m_rest{data}, m_total{data.size()} {}

    void read(std::span<std::byte> dst)
    {
        if (dst.size() > m_rest.size()) throw std::ios_base::failure("TxSegmentRecorder::read(): end of data");
        if (!dst.empty()) std::memcpy(dst.data(), m_rest.data(), dst.size());
        m_rest = m_rest.subspan(dst.size());
    }
    void ignore(size_t num_bytes)
    {
        if (num_bytes > m_rest.size()) throw std::ios_base::failure("TxSegmentRecorder::ignore(): end of data");
        m_rest = m_rest.subspan(num_bytes);
    }
    size_t size() const { return m_rest.size(); }
    bool empty() const { return m_rest.empty(); }
    size_t Pos() const { return m_total - m_rest.size(); }

    template <typename T>
    TxSegmentRecorder& operator>>(T&& obj)
    {
        ::Unserialize(*this, obj);
        return *this;
    }

    void Begin(TxSegments& segments)
    {
        m_segments = &segments;
        segments.begin = m_segment_start = Pos();
    }
    void End()
    {
        Close(Pos());
        m_segments->end = Pos();
        m_segments = nullptr;
    }

    void TxidHold() { m_hold_start = Pos(); }
    void TxidCommit() {}
    void TxidDiscard()
    {
        m_segments->has_witness = true;
        Close(m_hold_start);
        m_segment_start = Pos();
    }
    void TxidPause() { Close(Pos()); }
    void TxidResume() { m_segment_start = Pos(); }
};

//! The txid and wtxid of the transaction at `segments` in `data`: SHA256d over the recorded ranges. Inline, as the
//! parse that calls it is: out of line, through HashWriter, block deserialisation measured slower.
inline std::pair<Txid, Wtxid> HashTransactionSegments(std::span<const std::byte> data, const TxSegments& segments)
{
    const auto finish{[](CSHA256& hasher) {
        unsigned char out[CSHA256::OUTPUT_SIZE];
        hasher.Finalize(out);
        CSHA256{}.Write(out, sizeof(out)).Finalize(out);
        return uint256{out};
    }};
    const auto* bytes{reinterpret_cast<const unsigned char*>(data.data())};
    CSHA256 txid_hasher;
    for (size_t i{0}; i < segments.txid_count; ++i) {
        const auto [begin, end]{segments.txid[i]};
        txid_hasher.Write(bytes + begin, end - begin);
    }
    const Txid txid{Txid::FromUint256(finish(txid_hasher))};
    if (!segments.has_witness) return {txid, Wtxid::FromUint256(txid.ToUint256())};
    CSHA256 wtxid_hasher;
    wtxid_hasher.Write(bytes + segments.begin, segments.end - segments.begin);
    return {txid, Wtxid::FromUint256(finish(wtxid_hasher))};
}

#endif // BITCOIN_PRIMITIVES_TXSEGMENTS_H
