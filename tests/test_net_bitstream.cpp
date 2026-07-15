#include "doctest.h"

#include <Net/BitStream.h>

#include <cmath>
#include <string>

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
