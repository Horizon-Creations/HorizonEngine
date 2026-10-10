#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// ── "Is this the file I loaded?" ─────────────────────────────────────────────
// What the editor remembers about the scene file it has open, so that a change made
// BEHIND its back (a git pull, a checkout, a stash pop, another program) can be told
// from the editor's own save. ImGui-free and header-only, so a test can drive it
// against real files.
//
// The check is two-staged on purpose. Size and modification time are the cheap
// question, asked every second; they are only a hint, because git touches a file it
// rewrites with identical bytes and a sync tool can bump a time for nothing. Only
// when the hint says "different" is the content hashed, and the editor is told about
// a change only if the BYTES differ. A scene with a big landscape is a dozen
// megabytes of base64, so hashing is not something to do every second.
class SceneFileStamp
{
public:
	// Take the stamp of `path` as it is right now. An empty path, or a file that is
	// not there, leaves the stamp "nothing to watch" (changed() is then false).
	void remember(const std::string& path)
	{
		m_path = path;
		m_valid = false;
		if (path.empty()) return;
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec) return;
		const auto time = std::filesystem::last_write_time(path, ec);
		if (ec) return;
		m_size  = size;
		m_time  = time;
		m_hash  = hashFile(path);
		m_valid = true;
	}

	const std::string& path() const { return m_path; }
	bool watching() const { return m_valid; }

	// True when the file on disk holds different bytes than when it was remembered.
	// The stamp is NOT moved forward: call remember() once the change has been dealt
	// with (reloaded, or knowingly kept), or the answer stays true.
	bool changed() const
	{
		if (!m_valid) return false;
		std::error_code ec;
		const auto size = std::filesystem::file_size(m_path, ec);
		if (ec) return false;               // gone: the editor's own Save will recreate it
		const auto time = std::filesystem::last_write_time(m_path, ec);
		if (ec) return false;
		if (size == m_size && time == m_time) return false;
		return hashFile(m_path) != m_hash;  // a touch with identical bytes is not a change
	}

	// Same bytes, new time (a touch, a checkout of identical content): move the cheap
	// part of the stamp so the next poll does not hash again.
	void refreshTimes()
	{
		if (!m_valid) return;
		std::error_code ec;
		const auto time = std::filesystem::last_write_time(m_path, ec);
		if (!ec) m_time = time;
	}

	// 64-bit FNV-1a over the whole file. Not cryptographic; it only has to notice that
	// two versions of a scene differ.
	static std::uint64_t hashFile(const std::string& path)
	{
		std::ifstream in(std::filesystem::path(path), std::ios::binary);
		std::uint64_t h = 1469598103934665603ull;
		char buf[1 << 16];
		while (in)
		{
			in.read(buf, sizeof(buf));
			const std::streamsize n = in.gcount();
			for (std::streamsize i = 0; i < n; ++i)
			{
				h ^= static_cast<unsigned char>(buf[i]);
				h *= 1099511628211ull;
			}
		}
		return h;
	}

private:
	std::string                      m_path;
	std::uintmax_t                   m_size = 0;
	std::filesystem::file_time_type  m_time{};
	std::uint64_t                    m_hash = 0;
	bool                             m_valid = false;
};
