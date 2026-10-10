#include "HeprojRegistration.h"

#include <Diagnostics/Log.h>
#include <Platform/Process.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	#include <shlobj.h>   // SHChangeNotify
#else
	#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace HeprojRegistration
{
namespace
{
	bool iequals(const std::string& a, const std::string& b)
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (std::tolower(static_cast<unsigned char>(a[i])) !=
			    std::tolower(static_cast<unsigned char>(b[i])))
				return false;
		return true;
	}

	std::string trim(const std::string& s)
	{
		size_t b = 0, e = s.size();
		while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
		while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
		return s.substr(b, e - b);
	}

	// A path as UTF-8, which is what the registry strings and the log carry. On
	// Windows string() would be the ANSI code page and lose every letter outside it.
	std::string utf8(const fs::path& p)
	{
#ifdef _WIN32
		const std::wstring& w = p.native();
		if (w.empty()) return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
		                                  nullptr, 0, nullptr, nullptr);
		if (n <= 0) return {};
		std::string s(static_cast<size_t>(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
		                    nullptr, nullptr);
		return s;
#else
		return p.string();
#endif
	}

	bool readFile(const fs::path& p, std::string& out)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in) return false;
		std::ostringstream ss;
		ss << in.rdbuf();
		out = ss.str();
		return true;
	}

	// Temp file + rename: a reader (the desktop's database tools, another editor
	// starting at the same moment) never sees half a file.
	bool writeFileAtomic(const fs::path& p, const std::string& data, std::string& error)
	{
		fs::path tmp = p;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				error = "cannot write " + utf8(tmp);
				return false;
			}
			out.write(data.data(), static_cast<std::streamsize>(data.size()));
			out.flush();
			if (!out)
			{
				error = "cannot write " + utf8(tmp);
				return false;
			}
		}
		std::error_code ec;
		fs::rename(tmp, p, ec);
		if (ec)
		{
			std::error_code ignored;
			fs::remove(tmp, ignored);
			error = "cannot replace " + utf8(p) + ": " + ec.message();
			return false;
		}
		return true;
	}

	// Both files exist and hold the same bytes.
	bool sameBytes(const fs::path& a, const fs::path& b)
	{
		std::string x, y;
		return readFile(a, x) && readFile(b, y) && x == y;
	}

	// Source's bytes into destination, only when they differ — a current file is not
	// touched, not even its timestamp.
	bool copyIfDifferent(const fs::path& src, const fs::path& dst, std::string& error)
	{
		std::string data;
		if (!readFile(src, data))
		{
			error = "cannot read " + utf8(src);
			return false;
		}
		std::string have;
		if (readFile(dst, have) && have == data) return true;
		return writeFileAtomic(dst, data, error);
	}

	const char* const kAssets[] = {
		"horizon-editor.desktop", "horizon-editor.xml",
		"application-x-heproj.png", "horizon-editor.png",
	};
} // namespace

// ── The policy ───────────────────────────────────────────────────────────────

Outcome decide(bool enabled, bool filesPresent, State state)
{
	if (!enabled) return Outcome::Disabled;
	if (!filesPresent) return Outcome::MissingFiles;
	switch (state)
	{
	case State::Registered:    return Outcome::AlreadyRegistered;
	case State::NotRegistered: return Outcome::Registered;
	case State::Stale:         return Outcome::Updated;
	case State::Foreign:       return Outcome::LeftAlone;
	}
	return Outcome::Failed;
}

const char* keyword(Outcome o)
{
	switch (o)
	{
	case Outcome::Disabled:          return "disabled";
	case Outcome::Unsupported:       return "unsupported";
	case Outcome::MissingFiles:      return "missing-files";
	case Outcome::AlreadyRegistered: return "already-registered";
	case Outcome::Registered:        return "registered";
	case Outcome::Updated:           return "updated";
	case Outcome::LeftAlone:         return "left-alone";
	case Outcome::Failed:            return "failed";
	}
	return "unknown";
}

int exitCode(Outcome o)
{
	switch (o)
	{
	case Outcome::Disabled:
	case Outcome::AlreadyRegistered:
	case Outcome::Registered:
	case Outcome::Updated:      return 0;
	case Outcome::Failed:       return 1;
	case Outcome::LeftAlone:    return 3;
	case Outcome::MissingFiles: return 4;
	case Outcome::Unsupported:  return 5;
	}
	return 1;
}

std::string describe(const Result& r, const std::string& exe)
{
	switch (r.outcome)
	{
	case Outcome::Disabled:
		return ".heproj registration is switched off in Preferences; the file association was left as it is";
	case Outcome::Unsupported:
		return "this system registers .heproj through the application bundle; nothing to do here";
	case Outcome::MissingFiles:
		return "no FileTypes folder next to the editor (not the packaged layout); .heproj was not registered";
	case Outcome::AlreadyRegistered:
		return ".heproj already opens with this editor (" + exe + ")";
	case Outcome::Registered:
		return "registered .heproj project files with this editor (" + exe + ")";
	case Outcome::Updated:
		return "the .heproj registration was out of date; now it points at this editor (" + exe + ")";
	case Outcome::LeftAlone:
		return ".heproj is handled by " + (r.detail.empty() ? std::string("another application") : r.detail) +
		       "; left as it is (the FileTypes folder has a script that takes it over)";
	case Outcome::Failed:
		return "could not register .heproj: " + r.detail;
	}
	return {};
}

Plan plan(Backend& backend, bool enabled)
{
	Plan p;
	if (!enabled)
	{
		p.outcome = Outcome::Disabled;   // not even a look
		return p;
	}
	if (!backend.filesPresent())
	{
		p.outcome = decide(true, false, State::NotRegistered);
		return p;
	}
	State state = backend.observe(p.other);
	p.outcome = decide(true, true, state);
	if (p.outcome != Outcome::LeftAlone) p.other.clear();
	return p;
}

Result apply(Backend& backend, const Plan& p)
{
	Result r;
	r.outcome = p.outcome;
	if (p.outcome == Outcome::LeftAlone) r.detail = p.other;
	if (!p.needsWrite()) return r;
	std::string error;
	if (!backend.write(error))
	{
		r.outcome = Outcome::Failed;
		r.detail = error.empty() ? "the registration could not be written" : error;
	}
	return r;
}

Result ensureRegistered(Backend& backend, bool enabled)
{
	return apply(backend, plan(backend, enabled));
}

// ── Windows: what register_heproj.ps1 writes ─────────────────────────────────

std::vector<RegistryValue> windowsRegistryValues(const std::string& exe, const std::string& icon)
{
	using K = RegistryValue::Kind;
	const std::string ext  = "Software\\Classes\\.heproj";
	const std::string prog = std::string("Software\\Classes\\") + kProgId;
	return {
		{ ext,  "",             K::String, kProgId,    true  },
		{ ext,  "Content Type", K::String, kMimeType,  false },
		// REG_NONE with no data is how a value in OpenWithProgids says "this ProgID may open me".
		{ ext + "\\OpenWithProgids", kProgId, K::None, "", false },
		{ prog, "",             K::String, kTypeName,  false },
		{ prog + "\\DefaultIcon",         "", K::String, icon, true },
		// Both quoted: a path with spaces and a project path with spaces each arrive as ONE argument.
		{ prog + "\\shell\\open\\command", "", K::String, "\"" + exe + "\" \"%1\"", true },
	};
}

namespace
{
	class WindowsBackend final : public Backend
	{
	public:
		WindowsBackend(std::unique_ptr<Registry> registry, fs::path exe)
			: m_reg(std::move(registry)), m_exe(std::move(exe))
		{
			m_icon = m_exe.parent_path() / "FileTypes" / "heproj.ico";
		}

		std::string exePath() const override { return utf8(m_exe); }

		bool filesPresent() override
		{
			std::error_code ec;
			return fs::is_regular_file(m_icon, ec);
		}

		State observe(std::string& other) override
		{
			using Hive = Registry::Hive;
			// A choice made in Explorer ("Open with ... Always") outranks every
			// registration and is the clearest "somebody picked something else" there is.
			// The editor's own entries are not that.
			std::string choice;
			if (m_reg->readString(Hive::CurrentUser,
			        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.heproj\\UserChoice",
			        "ProgId", choice) &&
			    !choice.empty() && !iequals(choice, kProgId) &&
			    !iequals(choice, "Applications\\" + utf8(m_exe.filename())))
			{
				other = choice;
				return State::Foreign;
			}
			// The merged view: a machine-wide association by another program counts too.
			std::string ext;
			m_reg->readString(Hive::ClassesRoot, ".heproj", "", ext);
			if (!ext.empty() && !iequals(ext, kProgId))
			{
				other = ext;
				return State::Foreign;
			}
			if (ext.empty()) return State::NotRegistered;

			for (const RegistryValue& v : values())
			{
				if (!v.decides) continue;
				std::string have;
				if (!m_reg->readString(Hive::CurrentUser, v.subKey, v.name, have) || !iequals(have, v.data))
					return State::Stale;
			}
			return State::Registered;
		}

		bool write(std::string& error) override
		{
			for (const RegistryValue& v : values())
			{
				const bool ok = v.kind == RegistryValue::Kind::None
					? m_reg->writeNone(v.subKey, v.name)
					: m_reg->writeString(v.subKey, v.name, v.data);
				if (!ok)
				{
					error = "could not write HKCU\\" + v.subKey + (v.name.empty() ? "" : "\\" + v.name);
					return false;
				}
			}
			m_reg->notifyAssociationsChanged();
			return true;
		}

	private:
		std::vector<RegistryValue> values() const
		{
			return windowsRegistryValues(utf8(m_exe), utf8(m_icon));
		}

		std::unique_ptr<Registry> m_reg;
		fs::path                  m_exe;
		fs::path                  m_icon;
	};
} // namespace

std::unique_ptr<Backend> makeWindowsBackend(std::unique_ptr<Registry> registry, const fs::path& exe)
{
	return std::make_unique<WindowsBackend>(std::move(registry), exe);
}

// ── Linux: what install_file_types.sh writes ─────────────────────────────────

fs::path resolveXdgHome(const char* xdgValue, const char* home, const char* defaultBelowHome)
{
	// The Desktop Entry spec wants an absolute XDG_DATA_HOME; a relative one is ignored.
	if (xdgValue && *xdgValue)
	{
		const fs::path p(xdgValue);
		if (p.is_absolute()) return p;
	}
	if (home && *home) return fs::path(home) / defaultBelowHome;
	return {};
}

XdgLayout xdgLayoutFromEnvironment(const fs::path& exe)
{
	XdgLayout l;
	l.exe        = exe;
	l.dataHome   = resolveXdgHome(std::getenv("XDG_DATA_HOME"), std::getenv("HOME"), ".local/share");
	l.configHome = resolveXdgHome(std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME"), ".config");
	return l;
}

bool renderDesktopEntry(const std::string& templateText, const std::string& exe,
                        std::string& out, std::string& error)
{
	// Inside the quotes of an Exec line the spec makes four characters special; a
	// path with one of them is refused rather than registered half-working. '%' is
	// not refused, it is doubled.
	if (exe.find_first_of("\"`$\\") != std::string::npos)
	{
		error = "the editor's path contains one of  \" ` $ \\  which a .desktop Exec line cannot carry: " + exe;
		return false;
	}
	std::string execField;
	for (const char c : exe)
	{
		if (c == '%') execField += "%%";
		else          execField += c;
	}
	out.clear();
	std::istringstream in(templateText);
	std::string line;
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("Exec=", 0) == 0)         out += "Exec=\"" + execField + "\" %f\n";
		else if (line.rfind("TryExec=", 0) == 0) out += "TryExec=" + exe + "\n";
		else                                     out += line + "\n";
	}
	return true;
}

std::string defaultHandlerIn(const std::string& text, const std::string& mimeType)
{
	std::istringstream in(text);
	std::string line;
	bool inDefaults = false;
	while (std::getline(in, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == '#') continue;
		if (line[0] == '[')
		{
			inDefaults = line == "[Default Applications]";
			continue;
		}
		if (!inDefaults) continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos || trim(line.substr(0, eq)) != mimeType) continue;
		// "a.desktop;b.desktop;": the first is the default, the rest are fallbacks.
		std::istringstream ids(line.substr(eq + 1));
		std::string id;
		while (std::getline(ids, id, ';'))
		{
			id = trim(id);
			if (!id.empty()) return id;
		}
		return {};
	}
	return {};
}

namespace
{
	class XdgBackend final : public Backend
	{
	public:
		explicit XdgBackend(XdgLayout layout) : m_l(std::move(layout)) {}

		std::string exePath() const override { return utf8(m_l.exe); }

		bool filesPresent() override
		{
			if (m_l.dataHome.empty()) return false;   // no home: nowhere to write
			std::error_code ec;
			for (const char* f : kAssets)
				if (!fs::is_regular_file(assets() / f, ec)) return false;
			return true;
		}

		State observe(std::string& other) override
		{
			// Who is the default for the type? Somebody else: hands off. (xdg-mime writes
			// the config folder's file; older versions wrote the data folder's.)
			const fs::path lists[] = {
				mimeapps(m_l.configHome),
				m_l.dataHome.empty() ? fs::path() : mimeapps(m_l.dataHome / "applications"),
			};
			for (const fs::path& list : lists)
			{
				std::string text;
				if (list.empty() || !readFile(list, text)) continue;
				const std::string id = defaultHandlerIn(text, kMimeType);
				if (id.empty()) continue;
				if (id != kDesktopId)
				{
					other = id;
					return State::Foreign;
				}
				break;   // ours: no need to look further
			}
			std::error_code ec;
			if (!fs::exists(desktopFile(), ec)) return State::NotRegistered;
			return upToDate() ? State::Registered : State::Stale;
		}

		bool write(std::string& error) override
		{
			std::string templateText, entry;
			if (!readFile(assets() / "horizon-editor.desktop", templateText))
			{
				error = "cannot read " + utf8(assets() / "horizon-editor.desktop");
				return false;
			}
			if (!renderDesktopEntry(templateText, utf8(m_l.exe), entry, error)) return false;

			std::error_code ec;
			for (const fs::path& dir : { desktopFile().parent_path(), mimeFile().parent_path(),
			                             mimeIcon().parent_path(), appIcon().parent_path() })
			{
				fs::create_directories(dir, ec);
				if (ec)
				{
					error = "cannot create " + utf8(dir) + ": " + ec.message();
					return false;
				}
			}
			if (!writeFileAtomic(desktopFile(), entry, error)) return false;
			if (!copyIfDifferent(assets() / "horizon-editor.xml", mimeFile(), error)) return false;
			if (!copyIfDifferent(assets() / "application-x-heproj.png", mimeIcon(), error)) return false;
			if (!copyIfDifferent(assets() / "horizon-editor.png", appIcon(), error)) return false;

			if (m_l.runHelpers) refreshDesktop();
			return true;
		}

	private:
		fs::path assets() const      { return m_l.exe.parent_path() / "FileTypes"; }
		fs::path desktopFile() const { return m_l.dataHome / "applications" / kDesktopId; }
		fs::path mimeFile() const    { return m_l.dataHome / "mime" / "packages" / "horizon-editor.xml"; }
		fs::path mimeIcon() const
		{
			return m_l.dataHome / "icons" / "hicolor" / "256x256" / "mimetypes" / "application-x-heproj.png";
		}
		fs::path appIcon() const
		{
			return m_l.dataHome / "icons" / "hicolor" / "256x256" / "apps" / "horizon-editor.png";
		}
		static fs::path mimeapps(const fs::path& dir)
		{
			return dir.empty() ? fs::path() : dir / "mimeapps.list";
		}

		// What is installed is exactly what writing now would install.
		bool upToDate() const
		{
			std::string templateText, expected, have, error;
			if (!readFile(assets() / "horizon-editor.desktop", templateText) ||
			    !renderDesktopEntry(templateText, utf8(m_l.exe), expected, error))
				return false;   // write() reports the reason
			if (!readFile(desktopFile(), have) || have != expected) return false;
			return sameBytes(assets() / "horizon-editor.xml", mimeFile()) &&
			       sameBytes(assets() / "application-x-heproj.png", mimeIcon()) &&
			       sameBytes(assets() / "horizon-editor.png", appIcon());
		}

		// Each helper if the system has it. None is required for the files to be in
		// place, and a minimal desktop without one must not make the registration fail.
		static void helper(const char* label, const char* tool, std::vector<std::string> args)
		{
			const std::optional<fs::path> exe = HE::Proc::which(tool);
			if (!exe)
			{
				HE_LOG_INFO(Editor, "HeprojRegistration: %s not found, so the %s step was skipped",
				            tool, label);
				return;
			}
			HE::Proc::Options o;
			o.exe       = *exe;
			o.args      = std::move(args);
			o.timeoutMs = 8000;
			const HE::Proc::Result r = HE::Proc::run(o);
			if (!r.ok())
				HE_LOG_WARN(Editor, "HeprojRegistration: '%s' reported a problem (exit %d%s); log out and in to refresh the desktop",
				            label, r.exitCode, r.timedOut ? ", timed out" : "");
		}

		void refreshDesktop() const
		{
			helper("mime database", "update-mime-database", { utf8(m_l.dataHome / "mime") });
			helper("desktop database", "update-desktop-database", { utf8(m_l.dataHome / "applications") });
			helper("icon cache", "gtk-update-icon-cache",
			       { "-q", "-t", "-f", utf8(m_l.dataHome / "icons" / "hicolor") });
			// THE handler rather than one of several, so a double-click does not ask.
			// (Reached only when nobody else is the default: observe() ruled that out.)
			helper("default application", "xdg-mime", { "default", kDesktopId, kMimeType });
		}

		XdgLayout m_l;
	};
} // namespace

std::unique_ptr<Backend> makeXdgBackend(XdgLayout layout)
{
	return std::make_unique<XdgBackend>(std::move(layout));
}

// ── This machine ─────────────────────────────────────────────────────────────

#ifdef _WIN32
namespace
{
	std::wstring toWide(const std::string& s)
	{
		if (s.empty()) return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		if (n <= 0) return {};
		std::wstring w(static_cast<size_t>(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
		return w;
	}

	std::string toUtf8(const std::wstring& w) { return utf8(fs::path(w)); }

	// The only Windows-only code in the policy: the registry calls themselves.
	class Win32Registry final : public Registry
	{
	public:
		bool readString(Hive hive, const std::string& subKey, const std::string& name,
		                std::string& out) override
		{
			const HKEY root = hive == Hive::ClassesRoot ? HKEY_CLASSES_ROOT : HKEY_CURRENT_USER;
			const std::wstring wsub  = toWide(subKey);
			const std::wstring wname = toWide(name);
			const wchar_t* valueName = wname.empty() ? nullptr : wname.c_str();
			// REG_EXPAND_SZ is read too, unexpanded: another program's association may
			// well store its default that way, and "missing" would read as "free to take".
			const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
			DWORD bytes = 0;
			if (RegGetValueW(root, wsub.c_str(), valueName, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
				return false;
			std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
			DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
			if (RegGetValueW(root, wsub.c_str(), valueName, flags, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
				return false;
			out = toUtf8(std::wstring(buffer.c_str()));
			return true;
		}

		bool writeString(const std::string& subKey, const std::string& name,
		                 const std::string& data) override
		{
			return set(subKey, name, REG_SZ, toWide(data), true);
		}

		bool writeNone(const std::string& subKey, const std::string& name) override
		{
			return set(subKey, name, REG_NONE, std::wstring(), false);
		}

		void notifyAssociationsChanged() override
		{
			SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
		}

	private:
		static bool set(const std::string& subKey, const std::string& name, DWORD type,
		                const std::wstring& data, bool hasData)
		{
			HKEY key = nullptr;
			const std::wstring wsub = toWide(subKey);
			if (RegCreateKeyExW(HKEY_CURRENT_USER, wsub.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
			                    KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
				return false;
			const std::wstring wname = toWide(name);
			const BYTE* bytes = hasData ? reinterpret_cast<const BYTE*>(data.c_str()) : nullptr;
			const DWORD size  = hasData ? static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)) : 0;
			const LSTATUS status = RegSetValueExW(key, wname.empty() ? nullptr : wname.c_str(), 0, type, bytes, size);
			RegCloseKey(key);
			return status == ERROR_SUCCESS;
		}
	};
} // namespace
#endif

std::unique_ptr<Backend> makeNativeBackend()
{
#if defined(_WIN32)
	std::wstring buffer(MAX_PATH, L'\0');
	for (;;)
	{
		const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (n == 0) return nullptr;
		if (n < buffer.size())
		{
			buffer.resize(n);
			break;
		}
		buffer.resize(buffer.size() * 2);
	}
	return makeWindowsBackend(std::make_unique<Win32Registry>(), fs::path(buffer));
#elif defined(__APPLE__)
	return nullptr;   // Info.plist of the .app
#else
	// The running binary, not argv[0]: a double-click through a symlink or a launcher
	// must register the editor itself, and a relative argv[0] means nothing later.
	std::error_code ec;
	const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
	if (ec || exe.empty()) return nullptr;
	return makeXdgBackend(xdgLayoutFromEnvironment(exe));
#endif
}

// ── The editor ───────────────────────────────────────────────────────────────

namespace
{
	std::mutex  g_workerMutex;
	std::thread g_worker;

	// Failed and LeftAlone are warnings, never errors: HE_LOG_ERROR is forwarded to the
	// notification bell, and neither is a failure of the editor.
	void logResult(const Result& r, const std::string& exe)
	{
		const std::string what = describe(r, exe);
		if (r.outcome == Outcome::Failed)
			HE_LOG_WARN(Editor, "HeprojRegistration: %s", what.c_str());
		else
			HE_LOG_INFO(Editor, "HeprojRegistration: %s", what.c_str());
	}
} // namespace

StartupReport startAtStartup(bool enabled)
{
	StartupReport report;
	std::shared_ptr<Backend> backend = makeNativeBackend();
	if (!backend)
	{
		report.result.outcome = Outcome::Unsupported;
		logResult(report.result, {});
		return report;
	}

	const Plan p = plan(*backend, enabled);
	report.result.outcome = p.outcome;
	report.result.detail  = p.other;
	if (!p.needsWrite())
	{
		logResult(report.result, backend->exePath());
		return report;
	}

	report.writing = true;
	std::lock_guard<std::mutex> lock(g_workerMutex);
	if (g_worker.joinable()) g_worker.join();
	g_worker = std::thread([backend, p]
	{
		HE::Log::setThreadName("HeprojReg");
		logResult(apply(*backend, p), backend->exePath());
	});
	return report;
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_workerMutex);
	if (g_worker.joinable()) g_worker.join();
}

int runCommandLine(int argc, char** argv)
{
	bool asked = false;
	for (int i = 1; i < argc; ++i)
		if (argv && argv[i] && std::strcmp(argv[i], kCommandLineFlag) == 0) asked = true;
	if (!asked) return -1;

	Result result;
	std::string exe;
	if (std::unique_ptr<Backend> backend = makeNativeBackend())
	{
		exe    = backend->exePath();
		result = ensureRegistered(*backend, true);
	}
	const std::string line = std::string(keyword(result.outcome)) + ": " + describe(result, exe);
	std::puts(line.c_str());
	std::fflush(stdout);
	return exitCode(result.outcome);
}

} // namespace HeprojRegistration
