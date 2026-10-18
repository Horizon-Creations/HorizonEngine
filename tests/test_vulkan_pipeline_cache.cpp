#include "doctest.h"
#include <Backends/Vulkan/VulkanPipelineCacheFile.h>

#include <cstdint>
#include <cstring>
#include <vector>

// The on-disk VkPipelineCache file (REN-808). A blob the renderer feeds back
// must belong to this GPU and driver and arrive whole; a stale or damaged file
// has to be a miss (empty cache, recompile), never a blob the driver might
// trip over. CI has no Vulkan device, so these rules are pinned here.

using HE::vkpc::DeviceId;
using HE::vkpc::Verdict;

namespace
{
DeviceId rtx()
{
	DeviceId d;
	d.vendorID = 0x10DE; d.deviceID = 0x2786; d.driverVersion = 0x8A5C8000;
	for (int i = 0; i < 16; ++i) d.uuid[i] = static_cast<uint8_t>(0xA0 + i);
	return d;
}

// What a driver returns from vkGetPipelineCacheData: its header, then data.
std::vector<uint8_t> driverBlob(const DeviceId& d, size_t payload = 200)
{
	std::vector<uint8_t> b(32 + payload);
	const uint32_t w[4] = { 32u, 1u, d.vendorID, d.deviceID };
	std::memcpy(b.data(), w, sizeof(w));
	std::memcpy(b.data() + 16, d.uuid, 16);
	for (size_t i = 32; i < b.size(); ++i) b[i] = static_cast<uint8_t>(i * 7);
	return b;
}

Verdict check(const DeviceId& d, const std::vector<uint8_t>& file)
{
	const uint8_t* blob = nullptr; size_t n = 0;
	return HE::vkpc::unwrap(d, file.data(), file.size(), blob, n);
}
}

TEST_CASE("Vulkan pipeline cache file: a blob round-trips unchanged")
{
	const DeviceId d = rtx();
	const auto blob = driverBlob(d);
	const auto file = HE::vkpc::wrap(d, blob.data(), blob.size());
	CHECK(file.size() == sizeof(HE::vkpc::FileHeader) + blob.size());

	const uint8_t* out = nullptr; size_t n = 0;
	REQUIRE(HE::vkpc::unwrap(d, file.data(), file.size(), out, n) == Verdict::Ok);
	REQUIRE(n == blob.size());
	CHECK(std::memcmp(out, blob.data(), n) == 0);
}

TEST_CASE("Vulkan pipeline cache file: a file of another GPU or driver is a miss")
{
	const DeviceId d = rtx();
	const auto file = HE::vkpc::wrap(d, driverBlob(d).data(), driverBlob(d).size());

	DeviceId other = d; other.deviceID ^= 1;
	CHECK(check(other, file) == Verdict::OtherDevice);
	other = d; other.vendorID = 0x1002;
	CHECK(check(other, file) == Verdict::OtherDevice);
	other = d; other.driverVersion += 1;   // a driver update keeps vendor/device
	CHECK(check(other, file) == Verdict::OtherDevice);
	other = d; other.uuid[15] ^= 0x10;
	CHECK(check(other, file) == Verdict::OtherDevice);
}

TEST_CASE("Vulkan pipeline cache file: damaged files are misses")
{
	const DeviceId d = rtx();
	const auto blob = driverBlob(d);
	const auto good = HE::vkpc::wrap(d, blob.data(), blob.size());

	CHECK(check(d, {}) == Verdict::Empty);

	auto f = good; f.resize(20);                         // shorter than our header
	CHECK(check(d, f) == Verdict::BadHeader);
	f = good; f[0] = 'X';                                 // magic
	CHECK(check(d, f) == Verdict::BadHeader);
	f = good; f.resize(good.size() - 1);                  // a write cut short
	CHECK(check(d, f) == Verdict::Truncated);
	f = good; f.push_back(0);                             // trailing garbage
	CHECK(check(d, f) == Verdict::Truncated);
	f = good; f[sizeof(HE::vkpc::FileHeader) + 100] ^= 0x01;   // one flipped payload bit
	CHECK(check(d, f) == Verdict::BadHash);
}

TEST_CASE("Vulkan pipeline cache file: the driver header inside must match the device")
{
	const DeviceId d = rtx();
	auto wrapped = [&](std::vector<uint8_t> blob) { return HE::vkpc::wrap(d, blob.data(), blob.size()); };

	auto b = driverBlob(d); b[4] = 2;                     // headerVersion
	CHECK(check(d, wrapped(b)) == Verdict::BadBlob);
	b = driverBlob(d); b[8] ^= 1;                         // vendorID
	CHECK(check(d, wrapped(b)) == Verdict::BadBlob);
	b = driverBlob(d); b[12] ^= 1;                        // deviceID
	CHECK(check(d, wrapped(b)) == Verdict::BadBlob);
	b = driverBlob(d); b[31] ^= 1;                        // pipelineCacheUUID
	CHECK(check(d, wrapped(b)) == Verdict::BadBlob);
	b = driverBlob(d); b[0] = 16;                         // headerSize below the spec's 32
	CHECK(check(d, wrapped(b)) == Verdict::BadBlob);
	b = driverBlob(d, 0); b.resize(20);                   // no room for the driver header
	CHECK(check(d, wrapped(b)) == Verdict::BadBlob);
	CHECK(check(d, wrapped(driverBlob(d, 0))) == Verdict::Ok);   // header only: a valid empty cache
}
