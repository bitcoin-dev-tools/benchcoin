// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <batchverify.h>
#include <script/batchsigcache.h>

bool BatchingCachingTransactionSignatureChecker::VerifySchnorrSignature(Span<const unsigned char> sig, const XOnlyPubKey& pubkey, const uint256& sighash) const
{
    uint256 entry;
    m_signature_cache.ComputeEntrySchnorr(entry, sighash, sig, pubkey);
    if (m_signature_cache.Get(entry, !GetStore())) return true;
    SigCacheCallback callback(m_signature_cache, entry);
    return m_batch->Add(sig, pubkey, sighash, callback);
}
