// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BATCHVERIFY_H
#define BITCOIN_BATCHVERIFY_H

#include <pubkey.h>
#include <script/sigcache.h>
#include <sync.h>
#include <uint256.h>

#include <vector>

class SigCacheCallback
{
private:
    SignatureCache& m_signature_cache;
    uint256 m_entry;

public:
    SigCacheCallback(SignatureCache& cache, uint256 entry) : m_signature_cache(cache), m_entry(entry) {}

    void operator()() { m_signature_cache.Set(m_entry); }
};

class Batch;
class BatchSchnorrVerifier
{
private:
    Batch* m_batch GUARDED_BY(m_batch_mutex);
    std::vector<SigCacheCallback> m_callbacks GUARDED_BY(m_batch_mutex);
    mutable Mutex m_batch_mutex;

    size_t m_batch_size;

    void ExecuteCallbacks() EXCLUSIVE_LOCKS_REQUIRED(m_batch_mutex);

public:
    BatchSchnorrVerifier();
    ~BatchSchnorrVerifier();

    bool Add(const Span<const unsigned char> sig, const XOnlyPubKey& pubkey, const uint256& sighash, SigCacheCallback callback) EXCLUSIVE_LOCKS_REQUIRED(!m_batch_mutex);
    bool Verify() EXCLUSIVE_LOCKS_REQUIRED(!m_batch_mutex);
};

#endif // BITCOIN_BATCHVERIFY_H
