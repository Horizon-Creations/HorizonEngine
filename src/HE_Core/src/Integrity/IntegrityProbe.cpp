#include <Integrity/IntegrityProbe.h>

#include <Crypto/Sha256.h>
#include <Diagnostics/Log.h>
#include <JobSystem/JobSystem.h>

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <tuple>
#include <utility>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace HE::Integrity {

namespace {

bool isLibraryName(const std::string& n)
{
	auto endsWith = [&](const char* suffix) {
		const std::size_t len = std::char_traits<char>::length(suffix);
		return n.size() > len && n.compare(n.size() - len, len, suffix) == 0;
	};
	// "libfoo.so.1.0" is a library too; the exporter's own routing only looks
	// at ".so", but a versioned soname beside the exe still belongs to the
	// program and must not slip through unhashed.
	return endsWith(".dll") || endsWith(".dylib") || endsWith(".so") ||
	       n.find(".so.") != std::string::npos;
}

bool entryLess(const Entry& a, const Entry& b)
{
	return std::tie(a.kind, a.name) < std::tie(b.kind, b.name);
}

std::string hex64(std::uint64_t v)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
	return buf;
}

} // namespace

const char* fileKindName(FileKind kind)
{
	switch (kind)
	{
	case FileKind::Executable: return "executable";
	case FileKind::Library:    return "library";
	case FileKind::Pak:        return "pak";
	}
	return "?";
}

std::filesystem::path currentExecutablePath()
{
#if defined(__APPLE__)
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buf(size, '\0');
	if (size == 0 || _NSGetExecutablePath(buf.data(), &size) != 0) return {};
	buf.resize(std::char_traits<char>::length(buf.c_str()));
	// The dyld answer may go through symlinks; the bytes on disk are what
	// count, so resolve them.
	char real[PATH_MAX];
	if (::realpath(buf.c_str(), real)) return std::filesystem::path(real);
	return std::filesystem::path(buf);
#elif defined(_WIN32)
	wchar_t buf[MAX_PATH]{};
	const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return {};
	return std::filesystem::path(buf);
#else
	std::error_code ec;
	const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
	return ec ? std::filesystem::path{} : p;
#endif
}

Manifest probe(const Inputs& in)
{
	Manifest m;
	std::uint8_t digest[Crypto::Sha256::kDigestSize];

	if (!in.executable.empty())
	{
		if (Crypto::Sha256::hashFile(in.executable, digest))
			m.entries.push_back({ FileKind::Executable, in.executable.filename().string(),
			                      Crypto::Sha256::toHex(digest) });
		else
			HE_LOG_WARN(AntiCheat, "IntegrityProbe: cannot read executable %s",
			            in.executable.string().c_str());
	}

	const std::string exeName = in.executable.filename().string();
	for (const auto& dir : in.libraryDirs)
	{
		std::error_code ec;
		std::filesystem::directory_iterator it(dir, ec);
		const std::filesystem::directory_iterator end;
		for (; !ec && it != end; it.increment(ec))
		{
			if (!it->is_regular_file(ec)) { ec.clear(); continue; }
			const std::string name = it->path().filename().string();
			if (!isLibraryName(name) || name == exeName) continue;
			// Two directories may both list a name (flat export: exe dir and
			// data dir are the same directory). Hash it once.
			const bool seen = std::any_of(m.entries.begin(), m.entries.end(), [&](const Entry& e) {
				return e.kind == FileKind::Library && e.name == name;
			});
			if (seen) continue;
			if (Crypto::Sha256::hashFile(it->path(), digest))
				m.entries.push_back({ FileKind::Library, name, Crypto::Sha256::toHex(digest) });
			else
				HE_LOG_WARN(AntiCheat, "IntegrityProbe: cannot read library %s",
				            it->path().string().c_str());
		}
	}

	for (const auto& pak : in.paks)
		m.entries.push_back({ FileKind::Pak, pak.name, hex64(pak.tocHash) });

	std::sort(m.entries.begin(), m.entries.end(), entryLess);
	return m;
}

std::vector<Difference> compare(const Manifest& host, const Manifest& guest)
{
	// Both are sorted by (kind, name): one merge pass finds every difference in
	// both directions. Sorting again here costs nothing and makes the function
	// safe for a manifest that arrived over the wire unsorted.
	std::vector<Entry> h = host.entries, g = guest.entries;
	std::sort(h.begin(), h.end(), entryLess);
	std::sort(g.begin(), g.end(), entryLess);

	std::vector<Difference> out;
	std::size_t i = 0, j = 0;
	while (i < h.size() || j < g.size())
	{
		if (j >= g.size() || (i < h.size() && entryLess(h[i], g[j])))
		{
			out.push_back({ Mismatch::MissingOnGuest, h[i].kind, h[i].name });
			++i;
		}
		else if (i >= h.size() || entryLess(g[j], h[i]))
		{
			out.push_back({ Mismatch::ExtraOnGuest, g[j].kind, g[j].name });
			++j;
		}
		else
		{
			if (h[i].hash != g[j].hash)
				out.push_back({ Mismatch::HashDiffers, h[i].kind, h[i].name });
			++i;
			++j;
		}
	}
	return out;
}

std::string describe(const Difference& d)
{
	std::string s = fileKindName(d.kind);
	s += ' ';
	s += d.name;
	switch (d.what)
	{
	case Mismatch::HashDiffers:    s += " differs";           break;
	case Mismatch::MissingOnGuest: s += " missing on client"; break;
	case Mismatch::ExtraOnGuest:   s += " not part of the build"; break;
	}
	return s;
}

// ── IntegrityProbe ──

IntegrityProbe& IntegrityProbe::instance()
{
	static IntegrityProbe s_probe;
	return s_probe;
}

IntegrityProbe::IntegrityProbe()  = default;
IntegrityProbe::~IntegrityProbe()
{
	// Never let a worker publish into a destroyed object at process exit.
	waitForJob();
}

// The job's publish() takes m_mutex, so a wait must never happen while the
// lock is held — take the future out under the lock, wait without it.
void IntegrityProbe::waitForJob()
{
	std::future<void> job;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		job = std::move(m_job);
	}
	if (job.valid()) job.wait();
}

void IntegrityProbe::start(Inputs in)
{
	std::lock_guard<std::mutex> lk(m_mutex);
	if (m_state.load(std::memory_order_acquire) == State::Running) return;
	m_state.store(State::Running, std::memory_order_release);
	// `in` is the job's private copy; nothing on the main thread is touched.
	m_job = globalPool().submit([this, in = std::move(in)] {
		publish(probe(in));
	}, "IntegrityProbe");
}

void IntegrityProbe::runNow(const Inputs& in)
{
	waitForJob();
	m_state.store(State::Running, std::memory_order_release);
	publish(probe(in));
}

void IntegrityProbe::setManifest(Manifest m)
{
	waitForJob();
	std::sort(m.entries.begin(), m.entries.end(), entryLess);
	publish(std::move(m));
}

void IntegrityProbe::reset()
{
	waitForJob();
	std::lock_guard<std::mutex> lk(m_mutex);
	m_manifest = {};
	m_state.store(State::Idle, std::memory_order_release);
}

Manifest IntegrityProbe::manifest() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_manifest;
}

void IntegrityProbe::publish(Manifest m)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_manifest = std::move(m);
	}
	m_state.store(State::Ready, std::memory_order_release);
	HE_LOG_INFO(AntiCheat, "IntegrityProbe: manifest ready, %zu entries", manifest().entries.size());
}

} // namespace HE::Integrity
