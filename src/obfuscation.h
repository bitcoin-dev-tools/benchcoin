// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OBFUSCATION_H
#define BITCOIN_OBFUSCATION_H

#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <span.h>
#include <util/check.h>
#include <cstring>
#include <climits>
#include <serialize.h>

class Obfuscation
{
public:
    static constexpr size_t SIZE_BYTES{sizeof(uint64_t)};
    Obfuscation(const uint64_t key) { PrecomputeRotations(key); }
    Obfuscation(const Span<const std::byte> key_span) : Obfuscation(ToUint64(key_span)) {}
    Obfuscation(const std::array<const std::byte, SIZE_BYTES>& key_arr) : Obfuscation(ToUint64(key_arr)) {}
    Obfuscation(const std::vector<uint8_t>& key_vec) : Obfuscation(MakeByteSpan(key_vec)) {}

    uint64_t Key() const { return rotations[0]; }
    operator bool() const { return Key() != 0; }
    void operator()(Span<std::byte> write, const size_t key_offset_bytes = 0) const
    {
        if (!*this) return;
        const uint64_t rot_key{rotations[key_offset_bytes % SIZE_BYTES]}; // Continue obfuscation from where we left off
        for (; write.size() >= SIZE_BYTES; write = write.subspan(SIZE_BYTES)) { // Process multiple bytes at a time
            Xor(write, rot_key, SIZE_BYTES);
        }
        switch (write.size()) { // Help the compiler specialize remaining cases
        case 0:  break;
        case 1:  Xor(write, rot_key, 1); break;
        case 2:  Xor(write, rot_key, 2); break;
        case 4:  Xor(write, rot_key, 4); break;
        case 7:  Xor(write, rot_key, 7); break;
        default: Xor(write, rot_key, write.size());
        }
    }

private:
    std::array<uint64_t, SIZE_BYTES> rotations; // Cached key rotations
    void PrecomputeRotations(const uint64_t key)
    {
        for (size_t i{0}; i < SIZE_BYTES; ++i) {
            size_t key_rotation_bits{CHAR_BIT * i};
            if constexpr (std::endian::native == std::endian::big) key_rotation_bits *= -1;
            rotations[i] = std::rotr(key, key_rotation_bits);
        }
    }

    static uint64_t ToUint64(const Span<const std::byte> key_span)
    {
        assert(key_span.size() == SIZE_BYTES);
        uint64_t key{};
        std::memcpy(&key, key_span.data(), SIZE_BYTES);
        return key;
    }

    static void Xor(Span<std::byte> write, const uint64_t key, const size_t size)
    {
        assert(size <= write.size());
        uint64_t raw{};
        std::memcpy(&raw, write.data(), size);
        raw ^= key;
        std::memcpy(write.data(), &raw, size);
    }
};

template <typename Stream> void Serialize(Stream& s, const Obfuscation& a)
{
    const uint64_t key = a.Key();
    std::vector<std::byte> bytes(Obfuscation::SIZE_BYTES);
    std::memcpy(bytes.data(), &key, bytes.size());
    Serialize(s, bytes);
}
template <typename Stream> void Unserialize(Stream& s, Obfuscation& a)
{
    std::vector<std::byte> key(Obfuscation::SIZE_BYTES);
    s >> key;
    a = Obfuscation{key};
}

#endif // BITCOIN_OBFUSCATION_H
