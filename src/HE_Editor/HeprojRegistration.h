#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ── The editor registers .heproj for itself ──────────────────────────────────
// Windows and Linux ship the editor as a ZIP / tarball with no installer, so the
// one thing a file association has to say — WHERE the editor is — is only known
// to the editor. FileTypes/register_heproj.cmd and install_file_types.sh used to
// be run by hand once after unpacking (and again after every move). This does
// the same thing at every start, by itself:
//  * Windows: the keys register_heproj.ps1 writes, under HKCU\Software\Classes
//    (per user, no administrator), then SHChangeNotify so Explorer redraws.
//  * Linux: the files install_file_types.sh writes into $XDG_DATA_HOME, then
//    update-mime-database and friends as far as the system has them.
//  * macOS: nothing. The Info.plist of the .app is the registration, and
//    LaunchServices reads it the first time the app runs (package_macos.sh).
//
// The rule that keeps this polite: it fixes what is OURS and leaves alone what is
// somebody else's. If .heproj is already handled by another application — the
// person chose it in "Open with", or another program claimed it — nothing is
// written; the caller says so once. The scripts stay as the fallback that DOES
// take over.
//
// Cheap on purpose. Looking is a few registry values or a few small files, and
// only a registration that is missing or different is written, on a worker thread
// (the Linux helpers can take a moment). A registration that is already right is
// not touched, not even its timestamps.
//
// Layout of this file:
//  * the decision (State → Outcome) is a pure function and the part worth testing;
//  * the platform side sits behind Backend, with the Windows registry behind a
//    second seam (Registry) and the Linux files behind plain directories, so the
//    whole policy runs on any machine against fakes and temp folders. Only the
//    thin Win32 calls in the .cpp are Windows-only.
// SDL- and ImGui-free, and it never touches the editor's settings: the caller
// reads the Preferences switch and passes it in.
namespace HeprojRegistration
{
	// One name for the type on every platform; tests/test_heproj_file_types.py holds
	// these to the macOS UTI (package_macos.sh), register_heproj.ps1 and the Linux
	// MIME XML. A disagreement is how a file opens in the wrong application.
	inline constexpr const char* kProgId    = "dev.horizoncreations.heproj";
	inline constexpr const char* kMimeType  = "application/x-heproj";
	inline constexpr const char* kTypeName  = "Horizon Engine Project";
	inline constexpr const char* kDesktopId = "horizon-editor.desktop";

	// HorizonEditor --register-file-types: the same registration, right now, without
	// a window. What CI runs against the packaged editor, and a one-line alternative
	// to the scripts. See runCommandLine.
	inline constexpr const char* kCommandLineFlag = "--register-file-types";

	// Keys in the editor's config.json (GlobalState custom config). The switch is
	// Edit > Preferences > Editor > File Types, on by default. The second remembers
	// WHICH other application the person was told about, so the notice comes once
	// per application and not at every start.
	inline constexpr const char* kSettingKey = "RegisterHeprojFileType";
	inline constexpr const char* kNoticeKey  = "HeprojForeignHandlerNoticed";

	// ── What the platform says about .heproj ─────────────────────────────────
	enum class State
	{
		NotRegistered,  // nothing of ours, and nobody else's either
		Registered,     // ours, and exactly what this editor would write
		Stale,          // ours, but pointing elsewhere or incomplete (editor moved, keys half gone)
		Foreign,        // another application handles .heproj
	};

	enum class Outcome
	{
		Disabled,           // the Preferences switch is off; nothing was even looked at
		Unsupported,        // macOS (the Info.plist does it) or a system without a backend
		MissingFiles,       // no FileTypes/ next to the editor: not the packaged layout
		AlreadyRegistered,  // right already; nothing was written
		Registered,         // was not registered; written now
		Updated,            // was ours but out of date (moved editor); rewritten
		LeftAlone,          // another application handles .heproj; nothing was written
		Failed,             // writing failed
	};

	// `detail`: the other application for LeftAlone, the reason for Failed.
	struct Result
	{
		Outcome     outcome = Outcome::Unsupported;
		std::string detail;
	};

	// The policy, with no I/O: given the switch, whether the files to register are
	// there and what the platform says, what happens. Registered and Updated are the
	// two outcomes that mean "write now".
	Outcome decide(bool enabled, bool filesPresent, State state);

	// Short stable word for the outcome ("already-registered"), the exit code of
	// --register-file-types (0 for every outcome that leaves .heproj opening with
	// this editor, 1 failed, 3 left alone, 4 missing files, 5 unsupported) and one
	// sentence for the log. `exe` is only used in the sentence.
	const char* keyword(Outcome o);
	int exitCode(Outcome o);
	std::string describe(const Result& r, const std::string& exe);

	// ── The platform seam ────────────────────────────────────────────────────
	class Backend
	{
	public:
		virtual ~Backend() = default;
		// The editor binary this registers, for messages.
		virtual std::string exePath() const = 0;
		// Whether everything needed to register is next to the editor (FileTypes/).
		virtual bool filesPresent() = 0;
		// Reads, never writes. `other` names the foreign application for Foreign.
		virtual State observe(std::string& other) = 0;
		// Writes the whole registration; false and `error` on failure.
		virtual bool write(std::string& error) = 0;
	};

	struct Plan
	{
		Outcome     outcome = Outcome::Unsupported;  // Registered / Updated: a write is due
		std::string other;                           // LeftAlone: who handles .heproj
		bool needsWrite() const
		{
			return outcome == Outcome::Registered || outcome == Outcome::Updated;
		}
	};

	// Looks (nothing at all when `enabled` is false) and decides.
	Plan plan(Backend& backend, bool enabled);
	// Carries a plan out: writes when one is due, else just answers.
	Result apply(Backend& backend, const Plan& p);
	// plan + apply.
	Result ensureRegistered(Backend& backend, bool enabled);

	// ── Windows: the registry ────────────────────────────────────────────────
	// As little of the registry as the registration needs, so the policy runs on a
	// map in a test. The real one (Win32) is built on Windows only.
	class Registry
	{
	public:
		enum class Hive
		{
			ClassesRoot,  // HKEY_CLASSES_ROOT: the merged view the shell reads
			CurrentUser,  // HKEY_CURRENT_USER: where the registration is written
		};
		virtual ~Registry() = default;
		// A REG_SZ value; name "" is the key's default. False when key or value is missing.
		virtual bool readString(Hive hive, const std::string& subKey, const std::string& name,
		                        std::string& out) = 0;
		// Written under HKEY_CURRENT_USER, key created if need be.
		virtual bool writeString(const std::string& subKey, const std::string& name,
		                         const std::string& data) = 0;
		// A REG_NONE value with no data: how OpenWithProgids says "this ProgID may open me".
		virtual bool writeNone(const std::string& subKey, const std::string& name) = 0;
		// SHChangeNotify(SHCNE_ASSOCCHANGED): icons refresh without a sign-out.
		virtual void notifyAssociationsChanged() = 0;
	};

	// One value of the registration, exactly what register_heproj.ps1 writes.
	struct RegistryValue
	{
		enum class Kind { String, None };
		std::string subKey;   // under HKCU, backslash separated
		std::string name;     // "" = the default value
		Kind        kind = Kind::String;
		std::string data;
		// The three values that decide where a double-click goes (which program, which
		// icon). Only these are compared to call a registration current; the content
		// type, the "Open with" entry and the type's display name are written with
		// them but not second-guessed, so a registration that opens the editor is not
		// rewritten over cosmetics.
		bool        decides = false;
	};
	std::vector<RegistryValue> windowsRegistryValues(const std::string& exeUtf8,
	                                                 const std::string& iconUtf8);

	// The registration for `exe` (whose FileTypes/heproj.ico must exist), over any
	// Registry. The native Windows build hands in the Win32 one.
	std::unique_ptr<Backend> makeWindowsBackend(std::unique_ptr<Registry> registry,
	                                            const std::filesystem::path& exe);

	// ── Linux: the XDG data folder ───────────────────────────────────────────
	struct XdgLayout
	{
		std::filesystem::path dataHome;    // where applications/, mime/ and icons/ go
		std::filesystem::path configHome;  // where mimeapps.list names the default handler
		std::filesystem::path exe;         // the editor; FileTypes/ is beside it
		// Run update-mime-database, xdg-mime and friends after writing. Off in tests,
		// which must not reach the real desktop database.
		bool                  runHelpers = true;
	};
	// $XDG_DATA_HOME / $XDG_CONFIG_HOME, or $HOME/.local/share / $HOME/.config. A
	// relative XDG_* value is ignored, as the Desktop Entry spec and the installer
	// script do. Empty when there is neither a usable XDG_* nor a HOME.
	std::filesystem::path resolveXdgHome(const char* xdgValue, const char* home,
	                                     const char* defaultBelowHome);
	XdgLayout xdgLayoutFromEnvironment(const std::filesystem::path& exe);
	std::unique_ptr<Backend> makeXdgBackend(XdgLayout layout);

	// The .desktop file: the template with its Exec= and TryExec= lines pointing at
	// `exe` and everything else verbatim (install_file_types.sh does the same). Exec
	// double-quotes the path with % doubled; a path with " ` $ or \ cannot be carried
	// by an Exec line and is refused (false, `error` says why).
	bool renderDesktopEntry(const std::string& templateText, const std::string& exe,
	                        std::string& out, std::string& error);

	// The first desktop id that mimeapps.list names as the default for the type, "".
	std::string defaultHandlerIn(const std::string& mimeappsText, const std::string& mimeType);

	// ── The editor ───────────────────────────────────────────────────────────
	// The backend for this machine; null where nothing is needed (macOS).
	std::unique_ptr<Backend> makeNativeBackend();

	struct StartupReport
	{
		Result result;          // what is known now; for a write due: Registered / Updated
		bool   writing = false; // a worker is writing, and logs its own result
	};
	// Called once, after the settings are loaded and before the first frame. Looks now
	// (cheap), writes on a worker only if something has to be written, logs what was
	// decided. The caller reacts to LeftAlone with a notice.
	StartupReport startAtStartup(bool enabled);
	// Joins the worker. Bounded: every helper it runs has a timeout.
	void shutdown();

	// `--register-file-types` anywhere in the arguments: register now, print
	// "<keyword>: <sentence>", return the exit code. -1 when the flag is not there,
	// i.e. the editor starts as usual. The Preferences switch is not consulted: this
	// is a request.
	int runCommandLine(int argc, char** argv);
}
