// Copyright (c) 2016-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/siphash.h>

#include <bit>

static void Xor64x2(uint64_t& hi, uint64_t& lo, const uint64_t hi_val, const uint64_t lo_val) noexcept
{
#ifdef __SIZEOF_INT128__
    __int128 combined{__int128{hi} << 64 | lo};
    combined ^= __int128{hi_val} << 64 | lo_val;
    hi = static_cast<uint64_t>(combined >> 64);
    lo = static_cast<uint64_t>(combined);
#else
    hi ^= hi_val;
    lo ^= lo_val;
#endif
}

static void SipRound(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) noexcept
{
    v0 += v1;
    v2 += v3;
    v1 = std::rotl(v1, 13) ^ v0;
    v3 = std::rotl(v3, 16) ^ v2;
    v2 += v1;
    v0 = std::rotl(v0, 32) + v3;
    v3 = std::rotl(v3, 21) ^ v0;
    v1 = std::rotl(v1, 17) ^ v2;
    v2 = std::rotl(v2, 32);
}

CSipHasher::CSipHasher(uint64_t k0, uint64_t k1)
{
    v[0] = C0 ^ k0;
    v[1] = C1 ^ k1;
    v[2] = C2 ^ k0;
    v[3] = C3 ^ k1;
    count = 0;
    tmp = 0;
}

CSipHasher& CSipHasher::Write(uint64_t data)
{
    uint64_t v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];

    assert(count % 8 == 0);

    v3 ^= data;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= data;

    v[0] = v0;
    v[1] = v1;
    v[2] = v2;
    v[3] = v3;

    count += 8;
    return *this;
}

CSipHasher& CSipHasher::Write(std::span<const unsigned char> data)
{
    uint64_t v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];
    uint64_t t = tmp;
    uint8_t c = count;

    while (data.size() > 0) {
        t |= uint64_t{data.front()} << 8 * (c % 8);
        c++;
        if ((c & 7) == 0) {
            v3 ^= t;
            SipRound(v0, v1, v2, v3);
            SipRound(v0, v1, v2, v3);
            v0 ^= t;
            t = 0;
        }
        data = data.subspan(1);
    }

    v[0] = v0;
    v[1] = v1;
    v[2] = v2;
    v[3] = v3;
    count = c;
    tmp = t;

    return *this;
}

uint64_t CSipHasher::Finalize() const
{
    uint64_t v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];

    uint64_t t = tmp | (uint64_t)count << 56;

    v3 ^= t;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= t;
    v2 ^= 0xFF;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

uint64_t SipHashUint256(uint64_t k0, uint64_t k1, const uint256& val)
{
    /* Specialized implementation for efficiency */
    uint64_t d = val.GetUint64(0);

    uint64_t v0 = CSipHasher::C0 ^ k0;
    uint64_t v1 = CSipHasher::C1 ^ k1;
    uint64_t v2 = CSipHasher::C2 ^ k0;
    uint64_t v3 = CSipHasher::C3 ^ k1 ^ d;

    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= d;
    d = val.GetUint64(1);
    v3 ^= d;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= d;
    d = val.GetUint64(2);
    v3 ^= d;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= d;
    d = val.GetUint64(3);
    v3 ^= d;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= d;
    v3 ^= uint64_t{4} << 59;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= uint64_t{4} << 59;
    v2 ^= 0xFF;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

/* Specialized implementation for efficiency */
uint64_t Uint256ExtraSipHasher::operator()(const uint256& val, uint32_t extra) const noexcept
{
    const uint64_t d[]{val.GetUint64(0), val.GetUint64(1), val.GetUint64(2), val.GetUint64(3), uint64_t{36} << 56 | extra};
    uint64_t v03[]{v[0], v[3] ^ d[0]};
    uint64_t v12[]{v[1], v[2]};
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    Xor64x2(v03[0], v03[1], d[0], d[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    Xor64x2(v03[0], v03[1], d[1], d[2]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    Xor64x2(v03[0], v03[1], d[2], d[3]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    Xor64x2(v03[0], v03[1], d[3], d[4]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    Xor64x2(v03[0], v12[1], d[4], 0xFF);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    SipRound(v03[0], v12[0], v12[1], v03[1]);
    return v03[0] ^ v03[1] ^ v12[0] ^ v12[1];
}
