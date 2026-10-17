#pragma once

// ─── Laying the staged artifact out as an install root ───────────────────────
// Turns the single file SelfUpdateCheck.h verified (a .zip, .tar.gz or .dmg,
// whichever CI packages for this platform) into a directory shaped exactly like
// an install root: the one ApplyPlan::payloadRoot names. Runs in the EDITOR,
// before it ever builds a plan or quits — HorizonUpdater never extracts anything.
//
// No archive library is linked in for this: each platform only ever extracts
// the artifact IT packaged (a Windows editor never sees a .tar.gz), so the OS's
// own tool is enough — bsdtar via Windows' System32\tar.exe, GNU tar on Linux,
// hdiutil + ditto for the macOS .dmg. See docs/editor-selfupdate-sftp-design.md.

#include "SelfUpdate/SuCommon.h"

#include <filesystem>
#include <string>

namespace HE::Su {

// Where the running build's own executable sits relative to the directory an
// ApplyPlan would call its `installRoot`, for a given platform key
// ("windows-x64" -> "HorizonEditor.exe", "linux-x64" -> "HorizonEditor",
// "macos-arm64" -> "Contents/MacOS/HorizonEditor"). An unrecognised key falls
// back to the flat Windows/Linux shape.
HE_SU_API std::string mainExecutableRelativePath(const std::string& platformKey);

// The directory that names the CURRENTLY RUNNING build's install root: the
// directory holding `currentExecutable` on Windows/Linux, or the .app bundle
// three levels above it (Contents/MacOS/<exe> -> the bundle) on macOS. Empty
// when `currentExecutable` does not look like it sits where `platformKey`
// expects (e.g. not inside a Contents/MacOS on a macOS platform key).
HE_SU_API std::filesystem::path installRootForRunningBuild(const std::filesystem::path& currentExecutable,
                                                            const std::string& platformKey);

struct ExtractResult
{
	bool ok = false;
	std::string error;
	std::filesystem::path payloadRoot;   // valid when ok: has mainExecutableRelativePath() inside
};

// Extracts/mounts `artifactFile` under a fresh subdirectory of `destParentDir`
// (created if missing) and returns the directory that is shaped like an install
// root for `platformKey`. `destParentDir` should be a scratch directory nothing
// else uses — a failed attempt can leave partial content under it, which the
// caller is free to fs::remove_all().
HE_SU_API ExtractResult extractStagedBuild(const std::filesystem::path& artifactFile,
                                           const std::filesystem::path& destParentDir,
                                           const std::string& platformKey);

} // namespace HE::Su
