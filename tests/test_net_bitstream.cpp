#include "doctest.h"

#include <Net/BitStream.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace HE::Net;

// ─── Round-trip of every primitive ───────────────────────────────────────────

TEST_CASE("BitStream: primitives round-trip in order")
{
    BitWriter w;
    w.writeBool(true);
    w.writeBool(false);
    w.writeByte(0xAB);
    w.writeUInt16(0x1234);
    w.writeUInt32(0xDEADBEEF);
    w.writeUInt64(0x0123456789ABCDEFull);
    w.writeFloat(3.14159f);
    w.writeString("horizon");

    const auto bytes = w.data();
    BitReader r(bytes);

    bool b0 = false, b1 = true;
    std::uint8_t  byte = 0;
    std::uint16_t u16  = 0;
    std::uint32_t u32  = 0;
    std::uint64_t u64  = 0;
    float         f    = 0.0f;
    std::string   s;

    CHECK(r.readBool(b0));   CHECK(b0 == true);
    CHECK(r.readBool(b1));   CHECK(b1 == false);
    CHECK(r.readByte(byte)); CHECK(byte == 0xAB);
    CHECK(r.readUInt16(u16)); CHECK(u16 == 0x1234);
    CHECK(r.readUInt32(u32)); CHECK(u32 == 0xDEADBEEF);
    CHECK(r.readUInt64(u64)); CHECK(u64 == 0x0123456789ABCDEFull);
    CHECK(r.readFloat(f));   CHECK(f == doctest::Approx(3.14159f));
    CHECK(r.readString(s));  CHECK(s == "horizon");
}

// ─── Sub-byte bit packing ─────────────────────────────────────────────────────

TEST_CASE("BitStream: arbitrary bit widths pack tightly")
{
    BitWriter w;
    w.writeBits(0b101, 3);
    w.writeBits(0b11, 2);
    w.writeBits(1, 1);
    // 3+2+1 = 6 bits → still a single byte.
    CHECK(w.bitCount() == 6);
    CHECK(w.data().size() == 1);

    const auto bytes = w.data();
    BitReader r(bytes);
    std::uint32_t a = 0, b = 0, c = 0;
    CHECK(r.readBits(a, 3)); CHECK(a == 0b101);
    CHECK(r.readBits(b, 2)); CHECK(b == 0b11);
    CHECK(r.readBits(c, 1)); CHECK(c == 1);
}

// ─── Quantization ─────────────────────────────────────────────────────────────

TEST_CASE("BitStream: float quantization is bounded and roughly lossless")
{
    BitWriter w;
    w.writeFloatQuantized(0.5f, 0.0f, 1.0f, 16);
    w.writeFloatQuantized(-90.0f, -180.0f, 180.0f, 16);

    const auto bytes = w.data();
    BitReader r(bytes);
    float mid = 0.0f, ang = 0.0f;
    CHECK(r.readFloatQuantized(mid, 0.0f, 1.0f, 16));
    CHECK(r.readFloatQuantized(ang, -180.0f, 180.0f, 16));

    CHECK(mid == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(ang == doctest::Approx(-90.0f).epsilon(0.01));
}

TEST_CASE("BitStream: quantization clamps out-of-range inputs")
{
    BitWriter w;
    w.writeFloatQuantized(5.0f,  0.0f, 1.0f, 8);   // above max
    w.writeFloatQuantized(-5.0f, 0.0f, 1.0f, 8);   // below min

    const auto bytes = w.data();
    BitReader r(bytes);
    float hi = 0.0f, lo = 0.0f;
    CHECK(r.readFloatQuantized(hi, 0.0f, 1.0f, 8));
    CHECK(r.readFloatQuantized(lo, 0.0f, 1.0f, 8));
    CHECK(hi == doctest::Approx(1.0f));
    CHECK(lo == doctest::Approx(0.0f));
}

// ─── Bounds safety ────────────────────────────────────────────────────────────

TEST_CASE("BitStream: reading past the end fails instead of over-reading")
{
    BitWriter w;
    w.writeByte(0x42);

    const auto bytes = w.data();
    BitReader r(bytes);
    std::uint8_t got = 0;
    CHECK(r.readByte(got));      // consumes the only byte
    CHECK(got == 0x42);

    std::uint32_t overflow = 123;
    CHECK_FALSE(r.readUInt32(overflow));  // nothing left → false
    CHECK(r.bitsRemaining() == 0);
}

TEST_CASE("BitStream: truncated string length is rejected")
{
    // Claim a 10-byte string but provide no payload bytes.
    BitWriter w;
    w.writeUInt16(10);

    const auto bytes = w.data();
    BitReader r(bytes);
    std::string s = "unchanged";
    CHECK_FALSE(r.readString(s));   // must fail, not read garbage
}

TEST_CASE("BitStream: empty string round-trips")
{
    BitWriter w;
    w.writeString("");
    const auto bytes = w.data();
    BitReader r(bytes);
    std::string s = "x";
    CHECK(r.readString(s));
    CHECK(s.empty());
}

// ─── Wire format against the bit-at-a-time original (NET-101) ────────────────
// writeBits/readBits work a byte at a time and writeBytes/readBytes copy whole
// runs when aligned. The bytes on the wire must not move by one bit for that,
// so the original bit loops are kept here verbatim as the reference and random
// sequences of every operation, aligned and not, are compared byte for byte.

namespace {

class RefBitWriter {
public:
    void writeBits(std::uint32_t value, int numBits) {
        if (numBits <= 0) return;
        if (numBits > 32) numBits = 32;
        for (int i = 0; i < numBits; ++i) {
            const std::uint32_t bit = (value >> i) & 1u;
            m_current |= static_cast<std::uint8_t>(bit << m_bitInByte);
            if (++m_bitInByte == 8) {
                m_bytes.push_back(m_current);
                m_current   = 0;
                m_bitInByte = 0;
            }
        }
    }
    void writeBytes(const void* data, std::size_t len) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < len; ++i) writeBits(p[i], 8);
    }
    std::size_t bitCount() const { return m_bytes.size() * 8 + m_bitInByte; }
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

class RefBitReader {
public:
    explicit RefBitReader(const std::vector<std::uint8_t>& v)
        : m_data(v.data()), m_bitCapacity(v.size() * 8) {}
    bool readBits(std::uint32_t& out, int numBits) {
        if (numBits <= 0) { out = 0; return true; }
        if (numBits > 32) numBits = 32;
        if (m_bitPos + static_cast<std::size_t>(numBits) > m_bitCapacity) return false;
        std::uint32_t v = 0;
        for (int i = 0; i < numBits; ++i) {
            const std::uint32_t b = (m_data[m_bitPos >> 3] >> (m_bitPos & 7)) & 1u;
            v |= (b << i);
            ++m_bitPos;
        }
        out = v;
        return true;
    }
    bool readBytes(void* dst, std::size_t len) {
        auto* p = static_cast<std::uint8_t*>(dst);
        for (std::size_t i = 0; i < len; ++i) {
            std::uint32_t v = 0;
            if (!readBits(v, 8)) return false;
            p[i] = static_cast<std::uint8_t>(v);
        }
        return true;
    }
    std::size_t bitsRemaining() const { return m_bitCapacity - m_bitPos; }

private:
    const std::uint8_t* m_data        = nullptr;
    std::size_t         m_bitCapacity = 0;
    std::size_t         m_bitPos      = 0;
};

// One random operation, applied to both writers alike. Every typed write is a
// writeBits/writeBytes underneath, so these two plus their widths cover them;
// bool/byte/u16/u32/u64/string are still called by name on the real writer.
struct Op {
    int kind = 0;                      // 0 bits, 1 bytes, 2 bool, 3 byte, 4 u16, 5 u32, 6 u64, 7 string
    int width = 0;                     // for bits: -2..34 (out-of-range clamps)
    std::uint64_t value = 0;           // all 64 bits random: high bits must be ignored
    std::vector<std::uint8_t> bytes;   // for bytes/string
};

std::vector<Op> randomOps(std::mt19937& rng, int count) {
    std::vector<Op> ops;
    for (int i = 0; i < count; ++i) {
        Op op;
        op.kind  = static_cast<int>(rng() % 8);
        op.width = static_cast<int>(rng() % 37) - 2;
        op.value = (static_cast<std::uint64_t>(rng()) << 32) | rng();
        const std::size_t n = rng() % 4 == 0 ? rng() % 600 : rng() % 12;
        for (std::size_t b = 0; b < n; ++b) op.bytes.push_back(static_cast<std::uint8_t>(rng()));
        ops.push_back(std::move(op));
    }
    return ops;
}

void applyNew(BitWriter& w, const Op& op) {
    switch (op.kind) {
    case 0: w.writeBits(static_cast<std::uint32_t>(op.value), op.width); break;
    case 1: w.writeBytes(op.bytes.data(), op.bytes.size()); break;
    case 2: w.writeBool((op.value & 1u) != 0); break;
    case 3: w.writeByte(static_cast<std::uint8_t>(op.value)); break;
    case 4: w.writeUInt16(static_cast<std::uint16_t>(op.value)); break;
    case 5: w.writeUInt32(static_cast<std::uint32_t>(op.value)); break;
    case 6: w.writeUInt64(op.value); break;
    case 7: w.writeString(std::string(op.bytes.begin(), op.bytes.end())); break;
    }
}

void applyRef(RefBitWriter& w, const Op& op) {
    switch (op.kind) {
    case 0: w.writeBits(static_cast<std::uint32_t>(op.value), op.width); break;
    case 1: w.writeBytes(op.bytes.data(), op.bytes.size()); break;
    case 2: w.writeBits((op.value & 1u) ? 1u : 0u, 1); break;
    case 3: w.writeBits(static_cast<std::uint8_t>(op.value), 8); break;
    case 4: w.writeBits(static_cast<std::uint16_t>(op.value), 16); break;
    case 5: w.writeBits(static_cast<std::uint32_t>(op.value), 32); break;
    case 6:
        w.writeBits(static_cast<std::uint32_t>(op.value & 0xFFFFFFFFu), 32);
        w.writeBits(static_cast<std::uint32_t>(op.value >> 32), 32);
        break;
    case 7:
        w.writeBits(static_cast<std::uint32_t>(op.bytes.size()), 16);
        w.writeBytes(op.bytes.data(), op.bytes.size());
        break;
    }
}

} // namespace

TEST_CASE("BitStream: random write sequences match the bit-loop original byte for byte")
{
    std::mt19937 rng(0xB175u);
    for (int round = 0; round < 400; ++round) {
        const std::vector<Op> ops = randomOps(rng, 1 + static_cast<int>(rng() % 40));
        BitWriter    w;
        RefBitWriter ref;
        for (const Op& op : ops) {
            applyNew(w, op);
            applyRef(ref, op);
            REQUIRE(w.bitCount() == ref.bitCount());
        }
        const std::vector<std::uint8_t> got = w.data();
        REQUIRE(got == ref.data());
        CHECK(w.byteSize() == got.size());

        std::vector<std::uint8_t> appended{ 0xEE };   // appendTo appends, keeps what was there
        w.appendTo(appended);
        REQUIRE(appended.size() == 1 + got.size());
        CHECK(appended[0] == 0xEE);
        CHECK(std::equal(got.begin(), got.end(), appended.begin() + 1));

        // Read it back with both readers in the same mixed widths: every field
        // and the final position agree.
        BitReader    r(got);
        RefBitReader rr(got);
        for (const Op& op : ops) {
            if (op.kind == 1 || op.kind == 7) {
                if (op.kind == 7) {
                    std::uint32_t a = 0, b = 0;
                    REQUIRE(r.readBits(a, 16) == rr.readBits(b, 16));
                    REQUIRE(a == b);
                }
                std::vector<std::uint8_t> a(op.bytes.size()), b(op.bytes.size());
                REQUIRE(r.readBytes(a.data(), a.size()) == rr.readBytes(b.data(), b.size()));
                REQUIRE(a == b);
                CHECK(a == op.bytes);
            } else {
                const int width = op.kind == 0 ? op.width
                                : op.kind == 2 ? 1 : op.kind == 3 ? 8
                                : op.kind == 4 ? 16 : 32;
                const int reads = op.kind == 6 ? 2 : 1;
                for (int i = 0; i < reads; ++i) {
                    std::uint32_t a = 0xFFFFFFFFu, b = 0xFFFFFFFFu;
                    REQUIRE(r.readBits(a, width) == rr.readBits(b, width));
                    REQUIRE(a == b);
                }
            }
            REQUIRE(r.bitsRemaining() == rr.bitsRemaining());
        }
    }
}

TEST_CASE("BitStream: a short readBytes reads the whole bytes there are, like the byte loop")
{
    // Every start bit 0..7 and lengths past the end: the same return value, the
    // same bytes copied, the same read position afterwards as the original.
    std::mt19937 rng(0x5EEDu);
    for (int round = 0; round < 300; ++round) {
        std::vector<std::uint8_t> buf(rng() % 24);
        for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
        const int skip = static_cast<int>(rng() % 8);
        const std::size_t len = rng() % (buf.size() + 4);

        BitReader    r(buf);
        RefBitReader rr(buf);
        std::uint32_t s1 = 0, s2 = 0;
        const bool skipped = r.readBits(s1, skip);
        REQUIRE(skipped == rr.readBits(s2, skip));
        std::vector<std::uint8_t> a(len, 0x77), b(len, 0x77);
        CHECK(r.readBytes(a.data(), len) == rr.readBytes(b.data(), len));
        CHECK(a == b);
        CHECK(r.bitsRemaining() == rr.bitsRemaining());
    }

    // Zero bytes into nowhere is fine, aligned or not.
    const std::vector<std::uint8_t> one{ 0x5A };
    BitReader r(one);
    CHECK(r.readBytes(nullptr, 0));
    std::uint32_t bit = 0;
    CHECK(r.readBits(bit, 3));
    CHECK(r.readBytes(nullptr, 0));
    CHECK(r.bitsRemaining() == 5);
}

TEST_CASE("BitStream: golden bytes for an unaligned bulk write")
{
    // Pinned by hand, not by the reference: 3 bits 0b101, then 0xFF 0x00 0x81.
    // LSB-first, so byte 0 = 0b11111'101, byte 1 = 0b00000'111, byte 2 =
    // 0b00001'000 and the last 3 bits of 0x81 (0b100) open byte 3.
    BitWriter w;
    w.writeBits(0b101u, 3);
    const std::uint8_t run[] = { 0xFF, 0x00, 0x81 };
    w.writeBytes(run, sizeof(run));
    CHECK(w.bitCount() == 27);
    const std::vector<std::uint8_t> want{ 0xFD, 0x07, 0x08, 0x04 };
    CHECK(w.data() == want);
}
