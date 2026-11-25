#include "GitCloneDialog.h"
#include "NotificationStore.h"   // the clone result is a notification
#include "EditorApplication.h"        // AppContext, GitController
#include "EditorHelp.h"               // "Source Control/<label>" for its controls
#include "EditorWidgets.h"            // pinDialogToEditorWindow, WrapText, buttons
#include "EditorTheme.h"
#include "GitCloneView.h"             // what the dialog looks like (the Hub's cards)
#include "GitMissingDialog.h"         // "what is missing" when git is not installed
#include "GitHubSignIn.h"             // the account the list and the clone use

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#include <shobjidl.h>
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace GitCloneDialog
{

namespace fs = std::filesystem;

namespace {

bool        s_openRequested = false;
bool        s_hasOpenRequest = false;
std::string s_openRequest;

} // namespace

void requestOpen() { s_openRequested = true; }

bool takeOpenRequest(std::string& heprojPath)
{
	if (!s_hasOpenRequest) return false;
	s_hasOpenRequest = false;
	heprojPath = std::move(s_openRequest);
	s_openRequest.clear();
	return true;
}

#ifdef HE_IMGUI_ENABLED
namespace {

constexpr float kDialogWidth = 720.0f;
constexpr float kListHeight  = 268.0f;

char s_filter[128]     = "";
char s_parentDir[1024] = "";
char s_folderName[256] = "";
std::string s_selected;          // fullName of the chosen repository
std::string s_formError;         // what the form itself refused, before git ran

// One clone started from this dialog. Everything the controller reports about
// a clone is only shown while this is set, so a previous run's result never
// resurfaces in a fresh dialog.
bool     s_cloneStarted  = false;
bool     s_resultHandled = false;
fs::path s_projectFile;          // the .heproj found in the finished clone

// The list loads itself once per open and once per sign-in (GitCloneView.h).
GitCloneView::AutoLoad s_autoLoad;

// The folder picker's answer. Not the AppContext dialog bridge: the Hub's
// create form and the New Project popup drain that one every frame into their
// own directory field, so a folder picked here would land there instead. SDL
// may run the callback on another thread, hence the mutex.
std::mutex  s_pickMutex;
std::string s_pickResult;
bool        s_pickReady = false;

void resetForm()
{
	s_filter[0]     = '\0';
	s_folderName[0] = '\0';
	s_selected.clear();
	s_formError.clear();
	s_cloneStarted  = false;
	s_resultHandled = false;
	s_projectFile.clear();
	s_autoLoad = {};
}

// The project file of a cloned tree: at the top level, or one folder down for a
// repository that keeps the project beside other things. Sorted so the choice is
// the same every time when a repository carries more than one.
fs::path findProjectFile(const fs::path& root)
{
	std::vector<fs::path> found;
	const auto scan = [&found](const fs::path& dir) {
		std::error_code ec;
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code fec;
			if (it->is_regular_file(fec) && it->path().extension() == ".heproj")
				found.push_back(it->path());
		}
	};
	scan(root);
	if (found.empty())
	{
		std::error_code ec;
		for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code dec;
			if (it->is_directory(dec) && it->path().filename() != ".git")
				scan(it->path());
		}
	}
	if (found.empty()) return {};
	std::sort(found.begin(), found.end());
	return found.front();
}

// Checked here, on the main thread, before git is asked: the worker would
// refuse the same folder, but only after the credential step, and a typo in the
// folder field deserves an answer in the same frame.
bool targetUsable(const fs::path& parent, const std::string& name, std::string& why)
{
	if (name.empty() || name == "." || name == ".." ||
	    name.find_first_of("/\\:") != std::string::npos)
	{
		why = "The folder name must be a single folder, without slashes.";
		return false;
	}
	std::error_code ec;
	if (!fs::is_directory(parent, ec))
	{
		why = "The folder " + parent.string() + " does not exist.";
		return false;
	}
	const fs::path target = parent / name;
	if (!fs::exists(target, ec)) return true;
	if (!fs::is_directory(target, ec))
	{
		why = target.string() + " is a file. Pick another name.";
		return false;
	}
	if (!fs::is_empty(target, ec))
	{
		why = target.string() + " is not empty. A clone needs an empty folder, "
		      "so pick another name or another place.";
		return false;
	}
	return true;
}

void browseForParent(AppContext& ctx)
{
#ifdef _WIN32
	IFileOpenDialog* pDlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
		CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg))))
	{
		DWORD dwOpts = 0;
		pDlg->GetOptions(&dwOpts);
		pDlg->SetOptions(dwOpts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		HWND hwnd = nullptr;
		if (ctx.window)
			hwnd = static_cast<HWND>(SDL_GetPointerProperty(
				SDL_GetWindowProperties(ctx.window->GetNativeWindow()),
				SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		if (SUCCEEDED(pDlg->Show(hwnd)))
		{
			IShellItem* pItem = nullptr;
			if (SUCCEEDED(pDlg->GetResult(&pItem)))
			{
				PWSTR pPath = nullptr;
				if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)))
				{
					const int len = WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
						nullptr, 0, nullptr, nullptr);
					if (len > 0 && len <= static_cast<int>(sizeof(s_parentDir)))
						WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
							s_parentDir, sizeof(s_parentDir), nullptr, nullptr);
					CoTaskMemFree(pPath);
				}
				pItem->Release();
			}
		}
		pDlg->Release();
	}
#else
	SDL_ShowOpenFolderDialog(
		[](void*, const char* const* filelist, int)
		{
			if (!filelist || !filelist[0]) return;
			std::lock_guard<std::mutex> lock(s_pickMutex);
			s_pickResult = filelist[0];
			s_pickReady  = true;
		},
		nullptr,
		ctx.window ? ctx.window->GetNativeWindow() : nullptr,
		s_parentDir[0] ? s_parentDir : nullptr,
		false);
#endif
}

// A sensible first folder: where the Hub creates projects, else next to the
// open project, else the home folder.
void seedParentDir(AppContext& ctx)
{
	if (s_parentDir[0] != '\0') return;
	std::string seed;
	if (ctx.hubProjectDir && ctx.hubProjectDir[0] != '\0')
		seed = ctx.hubProjectDir;
	else if (ctx.git && !ctx.git->projectRoot().empty())
		seed = ctx.git->projectRoot().parent_path().string();
	else
	{
#ifdef _WIN32
		const char* home = std::getenv("USERPROFILE");
#else
		const char* home = std::getenv("HOME");
#endif
		if (home) seed = home;
	}
	std::snprintf(s_parentDir, sizeof(s_parentDir), "%s", seed.c_str());
}

void errorText(const std::string& s)
{
	ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Bad);
	ImGui::TextWrapped("%s", s.c_str());
	ImGui::PopStyleColor();
}

} // namespace
#endif  // HE_IMGUI_ENABLED

void Draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	GitController* git = ctx.git;

	if (s_openRequested)
	{
		s_openRequested = false;
		if (git && !git->cloneBusy())
		{
			resetForm();
			seedParentDir(ctx);
			ImGui::OpenPopup("##CloneRepository");
		}
	}

	{
		std::lock_guard<std::mutex> lock(s_pickMutex);
		if (s_pickReady)
		{
			s_pickReady = false;
			std::snprintf(s_parentDir, sizeof(s_parentDir), "%s", s_pickResult.c_str());
			s_pickResult.clear();
		}
	}

	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal("##CloneRepository", nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	// Same scope as the Preferences page it pairs with: "Create & push" there,
	// "Clone" here, one chapter in the manual.
	HE::Ed::Help::Scope helpScope("Source Control");

	bool close = false;
	{
		// A fixed column for the same reason GitMissingDialog has one: this popup
		// auto-sizes, and wrapping at its own edge would make the width its own input.
		EditorWidgets::WrapText wrap(kDialogWidth - ImGui::GetStyle().WindowPadding.x);

		const float contentW = kDialogWidth - ImGui::GetStyle().WindowPadding.x * 2.0f;
		GitCloneView::drawHeader(ctx.fontSubheading, contentW);

		const bool gitMissing = ctx.gitProbe && !ctx.gitProbe->gitFound;
		const bool cloning    = git && git->cloneBusy();

		if (!git)
		{
			ImGui::TextDisabled("Source control is unavailable in this build.");
		}
		else if (gitMissing)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
			ImGui::TextWrapped("git was not found on this machine, and cloning needs it.");
			ImGui::PopStyleColor();
			// The remedy in place, the same one Preferences shows: raising the
			// startup dialog instead does not work once its first probe has
			// answered, and a second modal would have to replace this one anyway.
			GitMissingDialog::drawGitInstallRemedy(ctx);
			if (EditorWidgets::button("Recheck##git") && ctx.recheckGit) ctx.recheckGit();
		}
		else
		{
			// ── 1. Sign-in → the account ─────────────────────────────────────
			// The sign-in flow draws in place of everything below while it runs:
			// this dialog is a modal, and the sign-in's own modal would close it.
			const fs::path credRoot = GitHubSignIn::credentialRoot(ctx);
			GitHubSignIn::ensureChecked(credRoot);
			const bool flowShown = GitHubSignIn::drawInline(ctx);
			const bool signedIn  = !flowShown &&
				GitHubSignIn::account() == GitHubSignIn::Account::SignedIn;
			if (!flowShown)
			{
				if (!signedIn)
					GitHubSignIn::drawAccountRow(ctx, /*inlineFlow=*/true);
				else if (GitCloneView::drawAccountCard(GitHubSignIn::accountLogin(), cloning, contentW))
					GitHubSignIn::signOut(credRoot);
			}

			// Signed in is all it takes: the list loads on its own, no button to
			// find first. The clone then needs no token either — git finds the
			// sign-in in the same credential helper.
			if (GitCloneView::shouldAutoLoad(s_autoLoad, signedIn, GitHubSignIn::accountGeneration(),
			                                 git->listingRepos(), cloning))
			{
				git->requestListReposWithSignIn(credRoot);
				s_selected.clear();
			}

			// Why the list could not be loaded is a notification, said once per attempt;
			// the list box only says there is no list to pick from.
			static std::string s_listReported;
			if (git->listingRepos())
				s_listReported.clear();
			else if (signedIn && git->repoListLoaded() && !git->repoListError().empty() &&
			         s_listReported != git->repoListError())
			{
				s_listReported = git->repoListError();
				HE::Ed::notify(HE::Ed::NoteLevel::Important,
				               "Could not list your repositories", git->repoListError());
			}

			// ── 2. Pick one ──────────────────────────────────────────────────
			// Only for the account signed in now: the list of an account that
			// signed out is still in the controller, and must not be offered.
			const auto& repos = git->repoList();
			const HE::Sc::RepoListEntry* chosen = nullptr;
			if (signedIn)
			{
				const GitCloneView::ListState state =
					git->listingRepos() || !git->repoListLoaded() ? GitCloneView::ListState::Loading
					: git->repoListError().empty()                ? GitCloneView::ListState::Ready
					                                              : GitCloneView::ListState::Failed;
				const GitCloneView::ListAction la = GitCloneView::drawRepoList(
					state, repos, s_filter, sizeof(s_filter), s_selected, cloning, kListHeight);
				if (la == GitCloneView::ListAction::Refresh)
				{
					git->requestListReposWithSignIn(credRoot);
					s_selected.clear();
				}
				if (state == GitCloneView::ListState::Ready)
					for (const auto& r : repos)
						if (r.fullName == s_selected) { chosen = &r; break; }
				if (la == GitCloneView::ListAction::Picked && chosen)
				{
					std::snprintf(s_folderName, sizeof(s_folderName), "%s", chosen->name.c_str());
					s_formError.clear();
				}

				// ── 3. Where → clone ─────────────────────────────────────────
				const fs::path target = fs::path(s_parentDir) / s_folderName;
				const GitCloneView::TargetAction ta = GitCloneView::drawTarget(
					s_parentDir, sizeof(s_parentDir), s_folderName, sizeof(s_folderName),
					chosen != nullptr, cloning, chosen && chosen->defaultBranch.empty());
				if (ta == GitCloneView::TargetAction::Browse)
					browseForParent(ctx);
				if (ta == GitCloneView::TargetAction::Clone && chosen)
				{
					s_formError.clear();
					if (targetUsable(s_parentDir, s_folderName, s_formError))
					{
						// No token of its own: git reads the sign-in from the
						// credential helper the list came through.
						git->requestClone(chosen->cloneUrl, target, {});
						s_cloneStarted  = true;
						s_resultHandled = false;
						s_projectFile.clear();
					}
				}
				if (!s_formError.empty()) errorText(s_formError);
			}

			// ── 4. Progress and result ───────────────────────────────────────
			if (s_cloneStarted && cloning)
			{
				ImGui::Spacing();
				ImGui::TextColored(HE::Ed::Theme::Ok, "%s",
				                   git->cloneInfo().empty() ? "Starting…"
				                                            : git->cloneInfo().c_str());
				ImGui::TextDisabled("A clone cannot be stopped once it has started. Large "
				                    "LFS assets can take a while.");
			}
			else if (s_cloneStarted)
			{
				const fs::path& root = git->clonedRoot();
				if (!s_resultHandled)
				{
					s_resultHandled = true;
					if (!root.empty()) s_projectFile = findProjectFile(root);
					// The outcome is a notification, said once per result (a retry of the
					// LFS download resets s_resultHandled and says it again).
					if (!git->cloneError().empty())
						HE::Ed::notify(HE::Ed::NoteLevel::Important,
						               root.empty() ? "Clone failed" : "Clone finished with a problem",
						               git->cloneError(), root.string());
					else
						HE::Ed::notify(HE::Ed::NoteLevel::Info, "Repository cloned",
						               git->cloneInfo(), root.string());
					// Clean success with a project in it: straight on to it.
					if (!root.empty() && git->cloneError().empty() && !s_projectFile.empty())
					{
						s_openRequest    = s_projectFile.string();
						s_hasOpenRequest = true;
						close = true;
					}
				}
				if (!close)
				{
					ImGui::Spacing();
					// What happened is in the notification; what is left here is what the
					// buttons below act on.
					if (!git->cloneError().empty())
						ImGui::TextDisabled(root.empty() ? "The clone did not complete."
						                                 : "Some large files were not downloaded.");

					if (!root.empty())
					{
						if (s_projectFile.empty())
							ImGui::TextWrapped("No HorizonEngine project (.heproj) was found in "
							                   "%s. The files are there, the folder was left "
							                   "as cloned.", root.string().c_str());
						// The repository exists even when LFS failed: the
						// download can be finished, or the project opened as is.
						if (!git->cloneError().empty() &&
						    EditorWidgets::button("Download LFS assets again"))
						{
							git->requestCloneLfsPull();
							s_resultHandled = false;
						}
						if (!s_projectFile.empty())
						{
							if (!git->cloneError().empty()) ImGui::SameLine();
							if (EditorWidgets::primaryButton("Open project"))
							{
								s_openRequest    = s_projectFile.string();
								s_hasOpenRequest = true;
								close = true;
							}
						}
					}
				}
			}
		}

		ImGui::Dummy(ImVec2(0.0f, 6.0f));
		ImGui::BeginDisabled(git && git->cloneBusy());
		if (EditorWidgets::button("Close", ImVec2(100.0f, 0.0f))) close = true;
		ImGui::EndDisabled();
	}

	if (close)
	{
		if (git && !git->cloneBusy()) git->finishClone();
		// A sign-in shown in here goes with the dialog. A token already approved
		// on GitHub is still saved (GitHubSignIn::endFlow).
		GitHubSignIn::endFlow();
		resetForm();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace GitCloneDialog
