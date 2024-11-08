// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SCRIPT_BATCHSIGCACHE_H
#define BITCOIN_SCRIPT_BATCHSIGCACHE_H

#include <batchverify.h>
#include <script/sigcache.h>

class BatchingCachingTransactionSignatureChecker : public CachingTransactionSignatureChecker
{
private:
    BatchSchnorrVerifier* m_batch;

public:
    BatchingCachingTransactionSignatureChecker(const CTransaction* txToIn, unsigned int nInIn, const CAmount& amountIn, bool storeIn, SignatureCache& signature_cache, PrecomputedTransactionData& txdataIn, BatchSchnorrVerifier* batchIn) : CachingTransactionSignatureChecker(txToIn, nInIn, amountIn, storeIn, signature_cache, txdataIn), m_batch(batchIn) {}

    bool VerifySchnorrSignature(Span<const unsigned char> sig, const XOnlyPubKey& pubkey, const uint256& sighash) const override;
};


#endif // BITCOIN_SCRIPT_BATCHSIGCACHE_H
