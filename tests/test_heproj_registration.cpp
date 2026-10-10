#include "doctest.h"
#include "TestFsUtil.h"
#include "HeprojRegistration.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// The editor registering .heproj for itself at start (HeprojRegistration.h).
// The policy runs against fakes: a Backend that counts what it is asked, a registry
// that is a map (so the Windows half runs on every machine, and the keys it writes
// are compared with register_heproj.ps1's) and the Linux half against a temp
// folder with an explicit XDG layout. NOTHING here reaches the real registry, the
// real ~/.local/share or the real desktop database: runHelpers is off, no
// makeNativeBackend(), and runCommandLine is only ever called without its flag.

namespace fs = std::filesystem;
namespace HR = HeprojRegistration;
using HR::Outcome;
using HR::State;

namespace
{
	struct Sandbox
	{
		fs::path root;
		explicit Sandbox(const char* tag)
		{
			root = fs::temp_directory_path() / ("he_test_heproj_reg_" + std::string(tag));
			he_test::removeAllQuiet(root);
			fs::create_directories(root);
		}
		~Sandbox() { he_test::removeAllQuiet(root); }
	};

	std::string slurp(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::ostringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}
	void put(const fs::path& p, const std::string& text)
	{
		fs::create_directories(p.parent_path());
		std::ofstream(p, std::ios::binary) << text;
	}

	// ── A Backend that only counts ───────────────────────────────────────────
	struct FakeBackend : HR::Backend
	{
		bool        files   = true;
		State       state   = State::NotRegistered;
		std::string other;
		bool        writeOk = true;
		std::string error;
		int filesAsked = 0, observed = 0, written = 0;

		std::string exePath() const override { return "/fake/HorizonEditor"; }
		bool filesPresent() override { ++filesAsked; return files; }
		State observe(std::string& o) override { ++observed; o = other; return state; }
		bool write(std::string& e) override
		{
			++written;
			if (!writeOk) e = error;
			return writeOk;
		}
	};

	// ── The registry as a map ────────────────────────────────────────────────
	// `user` is HKCU, `machine` stands for HKLM; HKCR, which the shell reads, is
	// the two merged with the user's winning, as on Windows.
	struct RegData
	{
		std::map<std::string, std::string> user;      // "key|name" -> data
		std::map<std::string, std::string> machine;   // key relative to Software\Classes
		std::set<std::string>              userNone;  // REG_NONE values
		int  writes   = 0;
		int  notified = 0;
		bool failWrites = false;
	};
	std::string slot(const std::string& key, const std::string& name) { return key + "|" + name; }

	struct FakeRegistry : HR::Registry
	{
		std::shared_ptr<RegData> d;
		explicit FakeRegistry(std::shared_ptr<RegData> data) : d(std::move(data)) {}

		bool readString(Hive hive, const std::string& key, const std::string& name,
		                std::string& out) override
		{
			if (hive == Hive::CurrentUser)
			{
				auto it = d->user.find(slot(key, name));
				if (it == d->user.end()) return false;
				out = it->second;
				return true;
			}
			auto u = d->user.find(slot("Software\\Classes\\" + key, name));
			if (u != d->user.end()) { out = u->second; return true; }
			auto m = d->machine.find(slot(key, name));
			if (m == d->machine.end()) return false;
			out = m->second;
			return true;
		}
		bool writeString(const std::string& key, const std::string& name, const std::string& data) override
		{
			if (d->failWrites) return false;
			++d->writes;
			d->user[slot(key, name)] = data;
			return true;
		}
		bool writeNone(const std::string& key, const std::string& name) override
		{
			if (d->failWrites) return false;
			++d->writes;
			d->userNone.insert(slot(key, name));
			return true;
		}
		void notifyAssociationsChanged() override { ++d->notified; }
	};

	const char* const kExt  = "Software\\Classes\\.heproj";
	const char* const kProg = "Software\\Classes\\dev.horizoncreations.heproj";
	const char* const kUserChoice =
		"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.heproj\\UserChoice";

	// An editor folder with a space and parentheses in its name and the icon the
	// registration points at; `exe` is what the backend registers.
	struct WinRig
	{
		Sandbox                  sb;
		fs::path                 exe;
		std::shared_ptr<RegData> data = std::make_shared<RegData>();
		std::unique_ptr<HR::Backend> backend;

		explicit WinRig(const char* tag, bool withIcon = true) : sb(tag)
		{
			exe = sb.root / "Horizon Editor (x86)" / "HorizonEditor.exe";
			put(exe, "placeholder");
			if (withIcon) put(exe.parent_path() / "FileTypes" / "heproj.ico", "ico");
			backend = HR::makeWindowsBackend(std::make_unique<FakeRegistry>(data), exe);
		}
		std::string command() const { return "\"" + exe.string() + "\" \"%1\""; }
		std::string icon() const { return (exe.parent_path() / "FileTypes" / "heproj.ico").string(); }
		std::string user(const std::string& key, const std::string& name = "") const
		{
			auto it = data->user.find(slot(key, name));
			return it == data->user.end() ? std::string("<missing>") : it->second;
		}
	};
}

// ── The policy ───────────────────────────────────────────────────────────────

TEST_CASE("HeprojRegistration::decide: the switch beats everything, missing files beat the state")
{
	const State all[] = { State::NotRegistered, State::Registered, State::Stale, State::Foreign };
	for (const State s : all)
	{
		CHECK(HR::decide(false, true,  s) == Outcome::Disabled);
		CHECK(HR::decide(false, false, s) == Outcome::Disabled);
		CHECK(HR::decide(true,  false, s) == Outcome::MissingFiles);
	}
}

TEST_CASE("HeprojRegistration::decide: not yet / current / moved / somebody else's")
{
	CHECK(HR::decide(true, true, State::NotRegistered) == Outcome::Registered);
	CHECK(HR::decide(true, true, State::Registered)    == Outcome::AlreadyRegistered);
	CHECK(HR::decide(true, true, State::Stale)         == Outcome::Updated);
	CHECK(HR::decide(true, true, State::Foreign)       == Outcome::LeftAlone);
}

TEST_CASE("HeprojRegistration: keywords and exit codes are the documented contract")
{
	// CI reads these from `HorizonEditor --register-file-types`.
	struct Row { Outcome o; const char* word; int code; };
	const Row rows[] = {
		{ Outcome::Registered,        "registered",         0 },
		{ Outcome::Updated,           "updated",            0 },
		{ Outcome::AlreadyRegistered, "already-registered", 0 },
		{ Outcome::Failed,            "failed",             1 },
		{ Outcome::LeftAlone,         "left-alone",         3 },
		{ Outcome::MissingFiles,      "missing-files",      4 },
		{ Outcome::Unsupported,       "unsupported",        5 },
		{ Outcome::Disabled,          "disabled",           0 },
	};
	for (const Row& r : rows)
	{
		CHECK(std::string(HR::keyword(r.o)) == r.word);
		CHECK(HR::exitCode(r.o) == r.code);
	}
}

TEST_CASE("HeprojRegistration: the sentences name the editor, the other application and the reason")
{
	CHECK(HR::describe({ Outcome::Registered, "" }, "/e/HorizonEditor").find("/e/HorizonEditor") != std::string::npos);
	CHECK(HR::describe({ Outcome::LeftAlone, "VSCode.heproj" }, "").find("VSCode.heproj") != std::string::npos);
	CHECK(HR::describe({ Outcome::Failed, "disk full" }, "").find("disk full") != std::string::npos);
}

// ── ensureRegistered over a Backend that counts ──────────────────────────────

TEST_CASE("HeprojRegistration::ensureRegistered: switched off, nothing is even looked at")
{
	FakeBackend b;
	b.state = State::NotRegistered;
	const HR::Result r = HR::ensureRegistered(b, false);
	CHECK(r.outcome == Outcome::Disabled);
	CHECK(b.filesAsked == 0);
	CHECK(b.observed == 0);
	CHECK(b.written == 0);
}

TEST_CASE("HeprojRegistration::ensureRegistered: without the FileTypes files nothing is looked at or written")
{
	FakeBackend b;
	b.files = false;
	const HR::Result r = HR::ensureRegistered(b, true);
	CHECK(r.outcome == Outcome::MissingFiles);
	CHECK(b.observed == 0);
	CHECK(b.written == 0);
}

TEST_CASE("HeprojRegistration::ensureRegistered: already registered is not written again")
{
	FakeBackend b;
	b.state = State::Registered;
	const HR::Result r = HR::ensureRegistered(b, true);
	CHECK(r.outcome == Outcome::AlreadyRegistered);
	CHECK(b.observed == 1);
	CHECK(b.written == 0);
}

TEST_CASE("HeprojRegistration::ensureRegistered: not registered is written once")
{
	FakeBackend b;
	b.state = State::NotRegistered;
	const HR::Result r = HR::ensureRegistered(b, true);
	CHECK(r.outcome == Outcome::Registered);
	CHECK(b.written == 1);
}

TEST_CASE("HeprojRegistration::ensureRegistered: a registration for another path is rewritten")
{
	FakeBackend b;
	b.state = State::Stale;
	const HR::Result r = HR::ensureRegistered(b, true);
	CHECK(r.outcome == Outcome::Updated);
	CHECK(b.written == 1);
}

TEST_CASE("HeprojRegistration::ensureRegistered: somebody else's handler is left alone and named")
{
	FakeBackend b;
	b.state = State::Foreign;
	b.other = "SomeoneElse.Project";
	const HR::Result r = HR::ensureRegistered(b, true);
	CHECK(r.outcome == Outcome::LeftAlone);
	CHECK(r.detail == "SomeoneElse.Project");
	CHECK(b.written == 0);
}

TEST_CASE("HeprojRegistration::ensureRegistered: a write that fails is reported with its reason")
{
	FakeBackend b;
	b.state   = State::NotRegistered;
	b.writeOk = false;
	b.error   = "access denied";
	const HR::Result r = HR::ensureRegistered(b, true);
	CHECK(r.outcome == Outcome::Failed);
	CHECK(r.detail == "access denied");
}

TEST_CASE("HeprojRegistration::plan only carries the other application for LeftAlone")
{
	FakeBackend b;
	b.state = State::Stale;
	b.other = "stray";
	CHECK(HR::plan(b, true).other.empty());
	CHECK(HR::plan(b, true).needsWrite());
	b.state = State::Foreign;
	CHECK(HR::plan(b, true).other == "stray");
	CHECK_FALSE(HR::plan(b, true).needsWrite());
}

TEST_CASE("HeprojRegistration::runCommandLine does nothing without its flag")
{
	// (With the flag it would register this machine's real .heproj: never in a test.)
	char a0[] = "HorizonEditor";
	char a1[] = "/some/project.heproj";
	char a2[] = "--dump";
	char* none[] = { a0 };
	char* some[] = { a0, a1, a2 };
	CHECK(HR::runCommandLine(1, none) == -1);
	CHECK(HR::runCommandLine(3, some) == -1);
	CHECK(HR::runCommandLine(0, nullptr) == -1);
}

// ── Windows: the keys, over a map ────────────────────────────────────────────

TEST_CASE("HeprojRegistration: the Windows values are exactly what register_heproj.ps1 writes")
{
	const std::string exe  = "C:\\Program Files (x86)\\Horizon Editor\\HorizonEditor.exe";
	const std::string icon = "C:\\Program Files (x86)\\Horizon Editor\\FileTypes\\heproj.ico";
	const std::vector<HR::RegistryValue> v = HR::windowsRegistryValues(exe, icon);

	auto find = [&](const std::string& key, const std::string& name) -> const HR::RegistryValue*
	{
		for (const HR::RegistryValue& x : v)
			if (x.subKey == key && x.name == name) return &x;
		return nullptr;
	};

	REQUIRE(v.size() == 6);
	const HR::RegistryValue* ext = find(kExt, "");
	REQUIRE(ext);
	CHECK(ext->data == "dev.horizoncreations.heproj");
	CHECK(ext->decides);
	const HR::RegistryValue* ct = find(kExt, "Content Type");
	REQUIRE(ct);
	CHECK(ct->data == "application/x-heproj");
	CHECK_FALSE(ct->decides);
	const HR::RegistryValue* with = find(std::string(kExt) + "\\OpenWithProgids", "dev.horizoncreations.heproj");
	REQUIRE(with);
	CHECK(with->kind == HR::RegistryValue::Kind::None);
	const HR::RegistryValue* name = find(kProg, "");
	REQUIRE(name);
	CHECK(name->data == "Horizon Engine Project");
	const HR::RegistryValue* ic = find(std::string(kProg) + "\\DefaultIcon", "");
	REQUIRE(ic);
	CHECK(ic->data == icon);
	CHECK(ic->decides);
	const HR::RegistryValue* cmd = find(std::string(kProg) + "\\shell\\open\\command", "");
	REQUIRE(cmd);
	// Both quoted: the editor's path and the project's path each arrive as ONE argument.
	CHECK(cmd->data == "\"C:\\Program Files (x86)\\Horizon Editor\\HorizonEditor.exe\" \"%1\"");
	CHECK(cmd->decides);

	int deciding = 0;
	for (const HR::RegistryValue& x : v)
	{
		if (x.decides) ++deciding;
		// Per user, and nothing outside Software\Classes.
		CHECK(x.subKey.rfind("Software\\Classes\\", 0) == 0);
	}
	CHECK(deciding == 3);
}

TEST_CASE("HeprojRegistration (Windows): a fresh registry is registered, once")
{
	WinRig rig("win_fresh");
	const HR::Result first = HR::ensureRegistered(*rig.backend, true);
	CHECK(first.outcome == Outcome::Registered);
	CHECK(rig.data->notified == 1);   // Explorer was told
	CHECK(rig.user(kExt) == "dev.horizoncreations.heproj");
	CHECK(rig.user(kExt, "Content Type") == "application/x-heproj");
	CHECK(rig.data->userNone.count(slot(std::string(kExt) + "\\OpenWithProgids", "dev.horizoncreations.heproj")) == 1);
	CHECK(rig.user(kProg) == "Horizon Engine Project");
	CHECK(rig.user(std::string(kProg) + "\\DefaultIcon") == rig.icon());
	CHECK(rig.user(std::string(kProg) + "\\shell\\open\\command") == rig.command());

	const int writes = rig.data->writes;
	const HR::Result second = HR::ensureRegistered(*rig.backend, true);
	CHECK(second.outcome == Outcome::AlreadyRegistered);
	CHECK(rig.data->writes == writes);     // idempotent: not one write
	CHECK(rig.data->notified == 1);        // and no second notification
}

TEST_CASE("HeprojRegistration (Windows): the content type is cosmetic and not second-guessed")
{
	WinRig rig("win_cosmetic");
	REQUIRE(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::Registered);
	rig.data->user[slot(kExt, "Content Type")] = "text/tampered";
	CHECK(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::AlreadyRegistered);
	CHECK(rig.user(kExt, "Content Type") == "text/tampered");   // CI relies on this: "not rewritten"
}

TEST_CASE("HeprojRegistration (Windows): a moved editor gets its registration rewritten")
{
	WinRig rig("win_moved");
	REQUIRE(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::Registered);
	const std::string cmdKey = std::string(kProg) + "\\shell\\open\\command";
	rig.data->user[slot(cmdKey, "")] = "\"C:\\Somewhere Else (x86)\\HorizonEditor.exe\" \"%1\"";
	rig.data->user[slot(kExt, "Content Type")] = "text/tampered";

	CHECK(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::Updated);
	CHECK(rig.user(cmdKey) == rig.command());
	CHECK(rig.user(kExt, "Content Type") == "application/x-heproj");   // a repair writes all of it
}

TEST_CASE("HeprojRegistration (Windows): another spelling of the same path is current, a wrong icon path is repaired")
{
	WinRig rig("win_icon");
	REQUIRE(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::Registered);
	const std::string iconKey = std::string(kProg) + "\\DefaultIcon";

	// Paths are not case sensitive on Windows: a different spelling is the same registration.
	std::string upper = rig.command();
	for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	rig.data->user[slot(std::string(kProg) + "\\shell\\open\\command", "")] = upper;
	CHECK(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::AlreadyRegistered);

	rig.data->user[slot(iconKey, "")] = "C:\\gone\\heproj.ico";
	CHECK(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::Updated);
	CHECK(rig.user(iconKey) == rig.icon());
}

TEST_CASE("HeprojRegistration (Windows): another program's .heproj is left alone, and nothing is written")
{
	SUBCASE("a registration of its own")
	{
		WinRig rig("win_foreign_ext");
		rig.data->user[slot(kExt, "")] = "SomeoneElse.Project";
		const HR::Result r = HR::ensureRegistered(*rig.backend, true);
		CHECK(r.outcome == Outcome::LeftAlone);
		CHECK(r.detail == "SomeoneElse.Project");
		CHECK(rig.data->writes == 0);
		CHECK(rig.data->notified == 0);
		CHECK(rig.user(kExt) == "SomeoneElse.Project");
		CHECK(rig.user(kProg) == "<missing>");   // not even the ProgID
	}
	SUBCASE("a machine-wide association (HKCR is the merged view)")
	{
		WinRig rig("win_foreign_machine");
		rig.data->machine[slot(".heproj", "")] = "Vendor.Project";
		const HR::Result r = HR::ensureRegistered(*rig.backend, true);
		CHECK(r.outcome == Outcome::LeftAlone);
		CHECK(r.detail == "Vendor.Project");
		CHECK(rig.data->writes == 0);
	}
	SUBCASE("a choice made in Explorer (UserChoice)")
	{
		WinRig rig("win_foreign_choice");
		rig.data->user[slot(kUserChoice, "ProgId")] = "VSCode.heproj";
		const HR::Result r = HR::ensureRegistered(*rig.backend, true);
		CHECK(r.outcome == Outcome::LeftAlone);
		CHECK(r.detail == "VSCode.heproj");
		CHECK(rig.data->writes == 0);
	}
}

TEST_CASE("HeprojRegistration (Windows): the person choosing THIS editor in Explorer is not a foreign handler")
{
	SUBCASE("through its own ProgID")
	{
		WinRig rig("win_choice_ours");
		rig.data->user[slot(kUserChoice, "ProgId")] = "dev.horizoncreations.heproj";
		CHECK(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::Registered);
	}
	SUBCASE("through Open with > Always, which names the exe")
	{
		WinRig rig("win_choice_exe");
		rig.data->user[slot(kUserChoice, "ProgId")] = "Applications\\HorizonEditor.exe";
		CHECK(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::Registered);
	}
}

TEST_CASE("HeprojRegistration (Windows): a failing registry write is a Failed result naming the key")
{
	WinRig rig("win_fail");
	rig.data->failWrites = true;
	const HR::Result r = HR::ensureRegistered(*rig.backend, true);
	CHECK(r.outcome == Outcome::Failed);
	CHECK(r.detail.find("HKCU") != std::string::npos);
	CHECK(rig.data->notified == 0);
}

TEST_CASE("HeprojRegistration (Windows): no icon next to the editor means no registration")
{
	WinRig rig("win_noicon", false);
	CHECK(HR::ensureRegistered(*rig.backend, true).outcome == Outcome::MissingFiles);
	CHECK(rig.data->writes == 0);
}

// ── Linux: the files, in a temp folder ───────────────────────────────────────
// A Windows path has backslashes, which an Exec line cannot carry; the files half
// is the Linux half.
#ifndef _WIN32

namespace
{
	// The real templates the package ships: the test moves with them.
	fs::path linuxAssets()
	{
		return fs::path(__FILE__).parent_path().parent_path() / "scripts" / "linux_assets";
	}

	struct XdgRig
	{
		Sandbox  sb;
		fs::path editorDir;
		HR::XdgLayout layout;

		explicit XdgRig(const char* tag, const std::string& editorName = "Horizon Editor (x86)")
			: sb(tag)
		{
			layout.dataHome   = sb.root / "data home";
			layout.configHome = sb.root / "config";
			layout.runHelpers = false;
			editorDir = sb.root / editorName;
			install(editorDir);
		}
		// An editor folder: the binary and the FileTypes assets beside it.
		void install(const fs::path& dir)
		{
			put(dir / "HorizonEditor", "#!/bin/sh\n");
			for (const char* f : { "horizon-editor.desktop", "horizon-editor.xml",
			                       "application-x-heproj.png", "horizon-editor.png" })
				put(dir / "FileTypes" / f, slurp(linuxAssets() / f));
			layout.exe = dir / "HorizonEditor";
		}
		std::unique_ptr<HR::Backend> backend() const { return HR::makeXdgBackend(layout); }
		fs::path desktop() const { return layout.dataHome / "applications" / "horizon-editor.desktop"; }
		fs::path mime() const { return layout.dataHome / "mime" / "packages" / "horizon-editor.xml"; }
		fs::path mimeIcon() const
		{
			return layout.dataHome / "icons" / "hicolor" / "256x256" / "mimetypes" / "application-x-heproj.png";
		}
		fs::path appIcon() const
		{
			return layout.dataHome / "icons" / "hicolor" / "256x256" / "apps" / "horizon-editor.png";
		}
		std::vector<fs::path> written() const { return { desktop(), mime(), mimeIcon(), appIcon() }; }
		// The line with this prefix.
		static std::string line(const std::string& text, const std::string& prefix)
		{
			std::istringstream in(text);
			std::string l;
			while (std::getline(in, l))
				if (l.rfind(prefix, 0) == 0) return l;
			return "<missing>";
		}
	};
}

TEST_CASE("HeprojRegistration (Linux): the templates are in the repository the test reads")
{
	for (const char* f : { "horizon-editor.desktop", "horizon-editor.xml",
	                       "application-x-heproj.png", "horizon-editor.png" })
		CHECK_MESSAGE(fs::exists(linuxAssets() / f), (linuxAssets() / f).string());
}

TEST_CASE("HeprojRegistration (Linux): a fresh data folder gets the files install_file_types.sh writes")
{
	XdgRig rig("xdg_fresh");
	auto b = rig.backend();
	const HR::Result r = HR::ensureRegistered(*b, true);
	REQUIRE(r.outcome == Outcome::Registered);
	for (const fs::path& p : rig.written()) CHECK_MESSAGE(fs::is_regular_file(p), p.string());

	const std::string exe  = (rig.editorDir / "HorizonEditor").string();
	const std::string text = slurp(rig.desktop());
	// The path has a space and parentheses: quoted, and %f after it.
	CHECK(XdgRig::line(text, "Exec=")    == "Exec=\"" + exe + "\" %f");
	CHECK(XdgRig::line(text, "TryExec=") == "TryExec=" + exe);
	// Everything else in the entry is the template, untouched.
	CHECK(XdgRig::line(text, "MimeType=") == "MimeType=application/x-heproj;");
	CHECK(XdgRig::line(text, "Icon=")     == "Icon=horizon-editor");
	CHECK(slurp(rig.mime()) == slurp(linuxAssets() / "horizon-editor.xml"));
	CHECK(slurp(rig.mimeIcon()) == slurp(linuxAssets() / "application-x-heproj.png"));
	CHECK(slurp(rig.appIcon()) == slurp(linuxAssets() / "horizon-editor.png"));
}

TEST_CASE("HeprojRegistration (Linux): the template's other lines come through verbatim and in order")
{
	std::string out, error;
	REQUIRE(HR::renderDesktopEntry("[Desktop Entry]\r\n# a comment\r\nExec=HorizonEditor %f\r\n"
	                               "TryExec=HorizonEditor\r\nMimeType=application/x-heproj;",
	                               "/opt/he/HorizonEditor", out, error));
	CHECK(out == "[Desktop Entry]\n# a comment\nExec=\"/opt/he/HorizonEditor\" %f\n"
	             "TryExec=/opt/he/HorizonEditor\nMimeType=application/x-heproj;\n");
}

TEST_CASE("HeprojRegistration (Linux): a registration that is already right is not touched, not even its timestamps")
{
	XdgRig rig("xdg_idem");
	{ auto b = rig.backend(); REQUIRE(HR::ensureRegistered(*b, true).outcome == Outcome::Registered); }

	// Set an old time and read back what the file system kept (some round it), so the
	// comparison afterwards is against what is really there.
	const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
	std::map<fs::path, fs::file_time_type> stamps;
	for (const fs::path& p : rig.written())
	{
		fs::last_write_time(p, old);
		stamps[p] = fs::last_write_time(p);
	}

	auto b = rig.backend();
	CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::AlreadyRegistered);
	for (const fs::path& p : rig.written())
		CHECK_MESSAGE(fs::last_write_time(p) == stamps[p], p.string());
}

TEST_CASE("HeprojRegistration (Linux): a moved editor is re-pointed, and back again")
{
	XdgRig rig("xdg_moved");
	{ auto b = rig.backend(); REQUIRE(HR::ensureRegistered(*b, true).outcome == Outcome::Registered); }

	const fs::path other = rig.sb.root / "unpacked elsewhere";
	const fs::path first = rig.layout.exe;
	rig.install(other);
	{
		auto b = rig.backend();
		CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::Updated);
		CHECK(XdgRig::line(slurp(rig.desktop()), "Exec=") == "Exec=\"" + (other / "HorizonEditor").string() + "\" %f");
	}
	rig.layout.exe = first;
	{
		auto b = rig.backend();
		CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::Updated);
		CHECK(XdgRig::line(slurp(rig.desktop()), "Exec=") == "Exec=\"" + first.string() + "\" %f");
	}
}

TEST_CASE("HeprojRegistration (Linux): a missing or changed file is written back")
{
	XdgRig rig("xdg_repair");
	{ auto b = rig.backend(); REQUIRE(HR::ensureRegistered(*b, true).outcome == Outcome::Registered); }

	SUBCASE("the MIME XML is gone")
	{
		fs::remove(rig.mime());
		auto b = rig.backend();
		CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::Updated);
		CHECK(fs::is_regular_file(rig.mime()));
	}
	SUBCASE("an icon differs")
	{
		put(rig.mimeIcon(), "not the icon");
		auto b = rig.backend();
		CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::Updated);
		CHECK(slurp(rig.mimeIcon()) == slurp(linuxAssets() / "application-x-heproj.png"));
	}
	SUBCASE("the desktop entry was edited")
	{
		put(rig.desktop(), "[Desktop Entry]\nExec=other\n");
		auto b = rig.backend();
		CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::Updated);
		CHECK(XdgRig::line(slurp(rig.desktop()), "Exec=") ==
		      "Exec=\"" + (rig.editorDir / "HorizonEditor").string() + "\" %f");
	}
}

TEST_CASE("HeprojRegistration (Linux): another default application for the type is left alone, nothing is created")
{
	XdgRig rig("xdg_foreign");
	put(rig.layout.configHome / "mimeapps.list",
	    "[Added Associations]\napplication/x-heproj=horizon-editor.desktop;\n"
	    "[Default Applications]\ntext/plain=gedit.desktop;\napplication/x-heproj=other.desktop;\n");
	auto b = rig.backend();
	const HR::Result r = HR::ensureRegistered(*b, true);
	CHECK(r.outcome == Outcome::LeftAlone);
	CHECK(r.detail == "other.desktop");
	CHECK_FALSE(fs::exists(rig.layout.dataHome));
}

TEST_CASE("HeprojRegistration (Linux): this editor as the default, or an unrelated entry, is not a foreign handler")
{
	SUBCASE("ours is the default")
	{
		XdgRig rig("xdg_default_ours");
		put(rig.layout.configHome / "mimeapps.list",
		    "[Default Applications]\napplication/x-heproj=horizon-editor.desktop;\n");
		auto b = rig.backend();
		CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::Registered);
	}
	SUBCASE("only another type has a default")
	{
		XdgRig rig("xdg_default_other_type");
		put(rig.layout.configHome / "mimeapps.list", "[Default Applications]\ntext/plain=gedit.desktop;\n");
		auto b = rig.backend();
		CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::Registered);
	}
}

TEST_CASE("HeprojRegistration (Linux): a percent in the path is doubled for the Exec field codes")
{
	XdgRig rig("xdg_percent", "100% editor");
	auto b = rig.backend();
	REQUIRE(HR::ensureRegistered(*b, true).outcome == Outcome::Registered);
	const std::string exe = (rig.editorDir / "HorizonEditor").string();
	std::string doubled;
	for (const char c : exe) { doubled += c; if (c == '%') doubled += '%'; }
	CHECK(XdgRig::line(slurp(rig.desktop()), "Exec=") == "Exec=\"" + doubled + "\" %f");
	// TryExec is a plain path, not an Exec line: no doubling.
	CHECK(XdgRig::line(slurp(rig.desktop()), "TryExec=") == "TryExec=" + exe);
}

TEST_CASE("HeprojRegistration (Linux): a path an Exec line cannot carry is refused, not half registered")
{
	XdgRig rig("xdg_quote", "quote\"editor");
	auto b = rig.backend();
	const HR::Result r = HR::ensureRegistered(*b, true);
	CHECK(r.outcome == Outcome::Failed);
	CHECK(r.detail.find("cannot carry") != std::string::npos);
	CHECK_FALSE(fs::exists(rig.desktop()));
}

TEST_CASE("HeprojRegistration (Linux): without the FileTypes assets next to the editor nothing happens")
{
	XdgRig rig("xdg_noassets");
	fs::remove(rig.editorDir / "FileTypes" / "horizon-editor.xml");
	auto b = rig.backend();
	CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::MissingFiles);
	CHECK_FALSE(fs::exists(rig.layout.dataHome));
}

TEST_CASE("HeprojRegistration (Linux): no data folder means nowhere to write")
{
	XdgRig rig("xdg_nohome");
	rig.layout.dataHome.clear();
	auto b = rig.backend();
	CHECK(HR::ensureRegistered(*b, true).outcome == Outcome::MissingFiles);
}

TEST_CASE("HeprojRegistration::defaultHandlerIn reads the first default for the type")
{
	CHECK(HR::defaultHandlerIn("[Default Applications]\napplication/x-heproj=a.desktop;b.desktop;\n",
	                           "application/x-heproj") == "a.desktop");
	CHECK(HR::defaultHandlerIn("# note\n[Default Applications]\n  application/x-heproj = spaced.desktop ; \n",
	                           "application/x-heproj") == "spaced.desktop");
	CHECK(HR::defaultHandlerIn("[Added Associations]\napplication/x-heproj=a.desktop;\n",
	                           "application/x-heproj").empty());
	CHECK(HR::defaultHandlerIn("[Default Applications]\napplication/x-heproj-other=a.desktop;\n",
	                           "application/x-heproj").empty());
	CHECK(HR::defaultHandlerIn("", "application/x-heproj").empty());
}

TEST_CASE("HeprojRegistration::resolveXdgHome follows the Desktop Entry spec like the installer script")
{
	CHECK(HR::resolveXdgHome("/data", "/home/u", ".local/share") == fs::path("/data"));
	CHECK(HR::resolveXdgHome("relative/dir", "/home/u", ".local/share") == fs::path("/home/u/.local/share"));
	CHECK(HR::resolveXdgHome("", "/home/u", ".config") == fs::path("/home/u/.config"));
	CHECK(HR::resolveXdgHome(nullptr, "/home/u", ".config") == fs::path("/home/u/.config"));
	CHECK(HR::resolveXdgHome(nullptr, nullptr, ".config").empty());
}

#endif   // !_WIN32
