// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <batchverify.h>
#include <logging.h>
#include <pubkey.h>
#include <random.h>
#include <sync.h>

#include <secp256k1.h>
#include <secp256k1_batch.h>
#include <secp256k1_schnorrsig_batch.h>

class Batch
{
private:
    secp256k1_batch* m_batch;

public:
    Batch(secp256k1_batch* batch) : m_batch(batch) {}
    secp256k1_batch* get() const { return m_batch; }
};

BatchSchnorrVerifier::BatchSchnorrVerifier()
{
    unsigned char rnd[16];
    GetRandBytes(rnd);
    // This is the maximum number of scalar-point pairs on the batch for which
    // Strauss' algorithm, which is used in the secp256k1 implementation, is
    // still efficient.
    const size_t max_batch_size{106};
    secp256k1_batch* batch{secp256k1_batch_create(secp256k1_context_static, max_batch_size, rnd)};
    m_batch = new Batch(batch);
    m_callbacks.reserve(max_batch_size);
    m_batch_size = max_batch_size;
}

BatchSchnorrVerifier::~BatchSchnorrVerifier()
{
    if (m_batch) {
        (void)secp256k1_batch_destroy(secp256k1_context_static, m_batch->get());
        delete m_batch;
    }
}

void BatchSchnorrVerifier::ExecuteCallbacks()
{
    AssertLockHeld(m_batch_mutex);
    for (size_t i = 0; i < m_callbacks.size(); i++) {
        m_callbacks[i]();
    }
    m_callbacks.clear();
}

bool BatchSchnorrVerifier::Add(const Span<const unsigned char> sig, const XOnlyPubKey& pubkey, const uint256& sighash, SigCacheCallback callback)
{
    LOCK(m_batch_mutex);
    if (secp256k1_batch_usable(secp256k1_context_static, m_batch->get()) == 0) {
        LogPrintf("ERROR: BatchSchnorrVerifier m_batch unusable\n");
        return false;
    }

    secp256k1_xonly_pubkey pubkey_parsed;
    if (!secp256k1_xonly_pubkey_parse(secp256k1_context_static, &pubkey_parsed, pubkey.data())) return false;
    if (secp256k1_batch_add_schnorrsig(secp256k1_context_static, m_batch->get(), sig.data(), sighash.begin(), 32, &pubkey_parsed)) {
        if (m_callbacks.size() == m_batch_size) {
            // Batch was verified and cleared, cache now
            ExecuteCallbacks();
        } else {
            m_callbacks.push_back(callback);
        }
        return true;
    }

    return false;
}

bool BatchSchnorrVerifier::Verify()
{
    LOCK(m_batch_mutex);
    if (secp256k1_batch_verify(secp256k1_context_static, m_batch->get())) {
        // cache sigs
        ExecuteCallbacks();
        return true;
    }
    return false;
}
