#pragma once

// ─── HorizonNet Layer 2 — bit-level serialization ────────────────────────────
// Header-only on purpose: BitWriter/BitReader are tiny value types used on both
// sides of the wire (and, later, by both gameplay replication and editor
// collaboration). Keeping them inline avoids exporting std::vector across the
// DLL boundary and lets the optimizer inline the hot read/write paths.
//
// Layout: bits are packed LSB-first within each byte, bytes in order. All reads
// are bounds-checked and return false on underflow rather than reading past the
// buffer — consistent with the engine's "silent failures become hard failures"
// stance: a caller that ignores the bool corrupts nothing, it just gets zeros.

#include "Net/NetCommon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace HE::Net {

class BitWriter {
public:
    // Write the low `numBits` bits of `value` (numBits in [0,32]).
    void writeBits(std::uint32_t value, int numBits) {
        if (numBits <= 0) return;
        if (numBits > 32) numBits = 32;
        for (int i = 0; i < numBits; ++i) {
            const std::uint32_t bit = (value >> i) & 1u;
            m_current |= static_cast<std::uint8_t>(bit << m_bitInByte);
            if (++m_bitInByte == 8) {
                m_bytes.push_back(m_current);
                m_current    = 0;
                m_bitInByte  = 0;
            }
        }
    }

    void writeBool(bool v)          { writeBits(v ? 1u : 0u, 1); }
    void writeByte(std::uint8_t v)  { writeBits(v, 8); }
    void writeUInt16(std::uint16_t v){ writeBits(v, 16); }
    void writeUInt32(std::uint32_t v){ writeBits(v, 32); }

    void writeUInt64(std::uint64_t v) {
        writeBits(static_cast<std::uint32_t>(v & 0xFFFFFFFFu), 32);
        writeBits(static_cast<std::uint32_t>(v >> 32),         32);
    }

    void writeFloat(float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        writeUInt32(bits);
    }

    // Quantize a float in [min,max] to `bits` bits. Lossy but bandwidth-cheap —
    // the intended path for replicated positions/rotations (Layer 3a).
    void writeFloatQuantized(float v, float min, float max, int bits) {
        bits = std::clamp(bits, 1, 31);
        const float span = (max > min) ? (max - min) : 1.0f;
        float t = (v - min) / span;
        t = std::clamp(t, 0.0f, 1.0f);
        const std::uint32_t maxQ = (1u << bits) - 1u;
        const auto q = static_cast<std::uint32_t>(std::lround(t * static_cast<float>(maxQ)));
        writeBits(q, bits);
    }

    void writeBytes(const void* data, std::size_t len) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < len; ++i) writeByte(p[i]);
    }

    // Length-prefixed string (16-bit length, so up to 65535 bytes).
    void writeString(const std::string& s) {
        const auto len = static_cast<std::uint16_t>(
            std::min<std::size_t>(s.size(), 0xFFFFu));
        writeUInt16(len);
        writeBytes(s.data(), len);
    }

    // Total bits written so far (including the partial trailing byte).
    std::size_t bitCount() const { return m_bytes.size() * 8 + m_bitInByte; }

    // Serialized bytes; the final partial byte (if any) is zero-padded.
    std::vector<std::uint8_t> data() const {
        std::vector<std::uint8_t> out = m_bytes;
        if (m_bitInByte > 0) out.push_back(m_current);
        return out;
    }

private:
    std::vector<std::uint8_t> m_bytes;
    std::uint8_t              m_current   = 0;
    int                       m_bitInByte = 0;
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t sizeBytes)
        : m_data(data), m_bitCapacity(sizeBytes * 8) {}

    explicit BitReader(const std::vector<std::uint8_t>& v)
        : m_data(v.data()), m_bitCapacity(v.size() * 8) {}

    // BitReader is a non-owning view (like std::span). Constructing one from a
    // temporary vector would store a pointer into a buffer that dies at the end
    // of the full expression — a dangling read. Reject it at compile time.
    explicit BitReader(std::vector<std::uint8_t>&&) = delete;

    bool readBits(std::uint32_t& out, int numBits) {
        if (numBits <= 0) { out = 0; return true; }
        if (numBits > 32) numBits = 32;
        if (m_bitPos + static_cast<std::size_t>(numBits) > m_bitCapacity) return false;
        std::uint32_t v = 0;
        for (int i = 0; i < numBits; ++i) {
            const std::size_t byte = m_bitPos >> 3;
            const std::size_t bit  = m_bitPos & 7;
            const std::uint32_t b  = (m_data[byte] >> bit) & 1u;
            v |= (b << i);
            ++m_bitPos;
        }
        out = v;
        return true;
    }

    bool readBool(bool& out) {
        std::uint32_t v; if (!readBits(v, 1)) return false; out = (v != 0); return true;
    }
    bool readByte(std::uint8_t& out) {
        std::uint32_t v; if (!readBits(v, 8)) return false; out = static_cast<std::uint8_t>(v); return true;
    }
    bool readUInt16(std::uint16_t& out) {
        std::uint32_t v; if (!readBits(v, 16)) return false; out = static_cast<std::uint16_t>(v); return true;
    }
    bool readUInt32(std::uint32_t& out) { return readBits(out, 32); }

    bool readUInt64(std::uint64_t& out) {
        std::uint32_t lo, hi;
        if (!readBits(lo, 32) || !readBits(hi, 32)) return false;
        out = (static_cast<std::uint64_t>(hi) << 32) | lo;
        return true;
    }

    bool readFloat(float& out) {
        std::uint32_t bits; if (!readUInt32(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool readFloatQuantized(float& out, float min, float max, int bits) {
        bits = std::clamp(bits, 1, 31);
        std::uint32_t q; if (!readBits(q, bits)) return false;
        const std::uint32_t maxQ = (1u << bits) - 1u;
        const float t = static_cast<float>(q) / static_cast<float>(maxQ);
        const float span = (max > min) ? (max - min) : 1.0f;
        out = min + t * span;
        return true;
    }

    bool readBytes(void* dst, std::size_t len) {
        auto* p = static_cast<std::uint8_t*>(dst);
        for (std::size_t i = 0; i < len; ++i) {
            if (!readByte(p[i])) return false;
        }
        return true;
    }

    bool readString(std::string& out) {
        std::uint16_t len; if (!readUInt16(len)) return false;
        if (m_bitPos + static_cast<std::size_t>(len) * 8 > m_bitCapacity) return false;
        out.resize(len);
        return len == 0 ? true : readBytes(out.data(), len);
    }

    std::size_t bitsRemaining() const { return m_bitCapacity - m_bitPos; }
    bool        overflowed()    const { return m_bitPos > m_bitCapacity; }

private:
    const std::uint8_t* m_data       = nullptr;
    std::size_t         m_bitCapacity = 0;
    std::size_t         m_bitPos      = 0;
};

} // namespace HE::Net
