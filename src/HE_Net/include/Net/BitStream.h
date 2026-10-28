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
    //
    // A byte at a time rather than a bit at a time: as many bits as still fit
    // into the open byte, then whole bytes. Same LSB-first layout as the
    // bit loop this replaced; bits above `numBits` are never read.
    void writeBits(std::uint32_t value, int numBits) {
        if (numBits <= 0) return;
        if (numBits > 32) numBits = 32;
        while (numBits > 0) {
            const int take = std::min(8 - m_bitInByte, numBits);
            const std::uint32_t mask = (1u << take) - 1u;
            m_current |= static_cast<std::uint8_t>((value & mask) << m_bitInByte);
            value     >>= take;
            numBits    -= take;
            m_bitInByte += take;
            if (m_bitInByte == 8) {
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

    // Bulk bytes. Snapshot and asset chunks arrive here byte-aligned almost
    // always, and then they are appended verbatim. Unaligned, every byte
    // straddles the open byte and the next one: its low bits finish the open
    // byte, its high bits start the following one.
    void writeBytes(const void* data, std::size_t len) {
        if (len == 0) return;
        const auto* p = static_cast<const std::uint8_t*>(data);
        if (m_bitInByte == 0) {
            m_bytes.insert(m_bytes.end(), p, p + len);
            return;
        }
        const int shift = m_bitInByte;
        const std::size_t base = m_bytes.size();
        m_bytes.resize(base + len);
        std::uint8_t* out = m_bytes.data() + base;
        for (std::size_t i = 0; i < len; ++i) {
            out[i]    = static_cast<std::uint8_t>(m_current | (p[i] << shift));
            m_current = static_cast<std::uint8_t>(p[i] >> (8 - shift));
        }
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

    // Size of data() in bytes, partial trailing byte included.
    std::size_t byteSize() const { return m_bytes.size() + (m_bitInByte > 0 ? 1 : 0); }

    // Room for `bytes` more without reallocating, for a caller that knows how
    // big the message is about to get.
    void reserveBytes(std::size_t bytes) { m_bytes.reserve(m_bytes.size() + bytes); }

    // Serialized bytes; the final partial byte (if any) is zero-padded.
    std::vector<std::uint8_t> data() const {
        std::vector<std::uint8_t> out;
        appendTo(out);
        return out;
    }

    // The same bytes as data(), appended to `out` instead of copied into a
    // vector of their own — how a frame is put together around a payload.
    void appendTo(std::vector<std::uint8_t>& out) const {
        out.reserve(out.size() + byteSize());
        out.insert(out.end(), m_bytes.begin(), m_bytes.end());
        if (m_bitInByte > 0) out.push_back(m_current);
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
        // The rest of the current byte, then whole bytes — the mirror of
        // BitWriter::writeBits.
        std::uint32_t v = 0;
        int got = 0;
        while (got < numBits) {
            const int bit  = static_cast<int>(m_bitPos & 7);
            const int take = std::min(8 - bit, numBits - got);
            const std::uint32_t chunk =
                (static_cast<std::uint32_t>(m_data[m_bitPos >> 3]) >> bit) & ((1u << take) - 1u);
            v |= chunk << got;
            got      += take;
            m_bitPos += static_cast<std::size_t>(take);
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

    // Bulk bytes, memcpy when the read head sits on a byte boundary. Short of
    // data it behaves as the byte loop it replaced did: the whole bytes that
    // are there are read (and the head moves past them), then false.
    bool readBytes(void* dst, std::size_t len) {
        auto* p = static_cast<std::uint8_t*>(dst);
        const std::size_t n = std::min(len, (m_bitCapacity - m_bitPos) / 8);
        const std::uint8_t* src = m_data + (m_bitPos >> 3);
        const int shift = static_cast<int>(m_bitPos & 7);
        if (n == 0) {
            // nothing to copy (and dst may be null)
        } else if (shift == 0) {
            std::memcpy(p, src, n);
        } else {
            // n whole bytes fit, so src[n] holds the high bits of the last one.
            for (std::size_t i = 0; i < n; ++i) {
                p[i] = static_cast<std::uint8_t>((src[i] >> shift) | (src[i + 1] << (8 - shift)));
            }
        }
        m_bitPos += n * 8;
        return n == len;
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
