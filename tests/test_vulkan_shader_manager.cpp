// VulkanShaderManager: id-allocation safety net (review 4, findings 385/386).
//
// load() and createProgram() hand out ids from a function-local static counter.
// With no VkDevice set, load() still runs its full path (read the file, cast the
// bytes into a VkShaderModuleCreateInfo, skip only the actual vkCreateShaderModule
// call) and returns a real handle — so these cases need no Vulkan driver at all,
// CI included.
#include "doctest.h"
#include "TestFsUtil.h"
#include <Backends/Vulkan/VulkanShaderManager.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

namespace
{
// A SPIR-V-shaped but not actually valid blob: load() never inspects the
// content (no device, so vkCreateShaderModule is never called), only its size
// and address matter here. `extraBytes` makes the file size NOT a multiple of
// 4 to exercise the byte-buffer -> uint32_t path finding 386 touches on an
// unaligned tail, same as a real .spv a build step truncated would.
std::filesystem::path writeBlob(const std::filesystem::path& dir, const char* name, size_t words, int extraBytes = 0)
{
	std::filesystem::create_directories(dir);
	std::filesystem::path p = dir / name;
	std::vector<char> bytes(words * 4 + extraBytes);
	for (size_t i = 0; i < bytes.size(); ++i)
		bytes[i] = static_cast<char>(0x10 + (i % 64));
	std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return p;
}
}

TEST_CASE("VulkanShaderManager::load assigns unique ascending ids without a device")
{
	std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_vulkan_shader_manager";
	std::filesystem::path a = writeBlob(dir, "a.spv", 16);
	// Deliberately not a multiple of 4 bytes, like a .spv a failed cook left half-written.
	std::filesystem::path b = writeBlob(dir, "b.spv", 16, 3);

	VulkanShaderManager mgr;
	ShaderHandle h1 = mgr.load(a.string().c_str(), HE::ShaderType::Vertex);
	ShaderHandle h2 = mgr.load(b.string().c_str(), HE::ShaderType::Fragment);

	// load()'s id counter is a function-local static shared by every manager
	// in the process, so other test cases may have already advanced it —
	// only the relative sequence (unique, ascending by one) is the contract.
	CHECK(h1.id > 0);
	CHECK(h2.id == h1.id + 1);
	CHECK(h1.type == HE::ShaderType::Vertex);
	CHECK(h2.type == HE::ShaderType::Fragment);
	// No device set: load() takes the "device not set" branch, no module is
	// created and ready stays false — compile() then reports "not found".
	CHECK_FALSE(h1.ready);
	CHECK_FALSE(mgr.compile(h1));

	ShaderProgramHandle p1 = mgr.createProgram({ h1 });
	ShaderProgramHandle p2 = mgr.createProgram({ h1, h2 });
	CHECK(p1.id > 0);
	CHECK(p2.id == p1.id + 1);
	CHECK(p2.shaders.size() == 2);

	mgr.release(h1);
	mgr.release(h2);
	mgr.cleanup();

	he_test::removeAllQuiet(dir);
}

TEST_CASE("VulkanShaderManager::load hands out unique ids under concurrent callers")
{
	// The static nextId counter load() uses is shared by every manager
	// instance; this pins that several managers loading in parallel never
	// collide — the same way several loading screens or streaming workers
	// calling into the backend at once would.
	std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_vulkan_shader_manager_mt";
	std::filesystem::path file = writeBlob(dir, "shared.spv", 8);

	constexpr int kThreads = 8;
	constexpr int kPerThread = 200;
	std::vector<std::vector<unsigned int>> ids(kThreads);
	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back([&, t]() {
			VulkanShaderManager mgr;
			ids[t].reserve(kPerThread);
			for (int i = 0; i < kPerThread; ++i)
				ids[t].push_back(mgr.load(file.string().c_str(), HE::ShaderType::Vertex).id);
		});
	}
	for (auto& th : threads)
		th.join();

	std::vector<unsigned int> all;
	all.reserve(size_t(kThreads) * kPerThread);
	for (auto& v : ids)
		all.insert(all.end(), v.begin(), v.end());

	std::set<unsigned int> unique(all.begin(), all.end());
	CHECK(all.size() == static_cast<size_t>(kThreads * kPerThread));
	// Duplicates (or a 0) are exactly what a racy ++ on a shared counter produces.
	CHECK(unique.size() == all.size());
	CHECK(unique.count(0) == 0);

	he_test::removeAllQuiet(dir);
}
