#include "GitCloneDialog.h"
#include "EditorApplication.h"        // AppContext, GitController
#include "EditorHelp.h"               // "Source Control/<label>" for its controls
#include "EditorWidgets.h"            // pinDialogToEditorWindow, WrapText, buttons
#include "GitMissingDialog.h"         // "what is missing" when git is not installed

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#include <shobjidl.h>
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
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

constexpr float kDialogWidth = 620.0f;

// Token for listing and for the clone. Wiped, not cleared, whenever it has been
// handed on or the dialog closes — the bytes must go, not just the length.
char s_token[256]      = "";
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

// The folder picker's answer. Not the AppContext dialog bridge: the Hub's
// create form and the New Project popup drain that one every frame into their
// own directory field, so a folder picked here would land there instead. SDL
// may run the callback on another thread, hence the mutex.
std::mutex  s_pickMutex;
std::string s_pickResult;
bool        s_pickReady = false;

void wipeToken() { std::fill(std::begin(s_token), std::end(s_token), '\0'); }

void resetForm()
{
	wipeToken();
	s_filter[0]     = '\0';
	s_folderName[0] = '\0';
	s_selected.clear();
	s_formError.clear();
	s_cloneStarted  = false;
	s_resultHandled = false;
	s_projectFile.clear();
}

bool containsNoCase(const std::string& hay, const char* needle)
{
	if (!needle || !*needle) return true;
	const std::string n(needle);
	const auto it = std::search(hay.begin(), hay.end(), n.begin(), n.end(),
		[](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
	return it != hay.end();
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
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
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

		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::TextUnformatted("Clone a Repository from GitHub");
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Separator();
		ImGui::Spacing();

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
			if (EditorWidgets::button("What is missing?"))
			{
				GitMissingDialog::requestShow();
				if (ctx.recheckGit) ctx.recheckGit();
				close = true;
			}
		}
		else
		{
			ImGui::TextWrapped(
				"Pick a repository from your GitHub account. It is cloned into a new "
				"folder, Git LFS assets included, and opened as a project.");
			ImGui::Spacing();

			// ── 1. Token → repository list ───────────────────────────────────
			ImGui::BeginDisabled(cloning);
			ImGui::SetNextItemWidth(-190.0f);
			ImGui::InputTextWithHint("##clonetoken", "Personal access token",
			                         s_token, sizeof(s_token), ImGuiInputTextFlags_Password);
			ImGui::SameLine();
			ImGui::BeginDisabled(s_token[0] == '\0' || git->listingRepos());
			if (EditorWidgets::primaryButton("Load my repositories", ImVec2(180.0f, 0.0f)))
			{
				// A copy: the same token is needed again for the clone itself.
				git->requestListRepos(std::string(s_token));
				s_selected.clear();
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Token: github.com/settings/tokens — classic, 'repo' scope. "
			                    "It is handed to git's credential helper, stored nowhere else.");

			if (git->listingRepos())
			{
				ImGui::Spacing();
				ImGui::TextDisabled("Loading your repositories…");
			}
			else if (git->repoListLoaded() && !git->repoListError().empty())
			{
				ImGui::Spacing();
				errorText(git->repoListError());
			}

			// ── 2. Pick one ──────────────────────────────────────────────────
			const auto& repos = git->repoList();
			const HE::Sc::RepoListEntry* chosen = nullptr;
			if (git->repoListLoaded() && git->repoListError().empty() && !git->listingRepos())
			{
				ImGui::Spacing();
				ImGui::SeparatorText("Repository");
				if (repos.empty())
				{
					ImGui::TextWrapped("This account has no repositories yet.");
				}
				else
				{
					// A search field only once the list stops fitting at a glance.
					if (repos.size() > 8)
					{
						ImGui::SetNextItemWidth(-1.0f);
						ImGui::InputTextWithHint("##clonefilter", "Search…",
						                         s_filter, sizeof(s_filter));
					}
					ImGui::BeginDisabled(cloning);
					ImGui::BeginChild("##clonerepos", ImVec2(0.0f, 220.0f), true);
					int shown = 0;
					for (std::size_t i = 0; i < repos.size(); ++i)
					{
						const HE::Sc::RepoListEntry& r = repos[i];
						if (!containsNoCase(r.fullName, s_filter)) continue;
						++shown;
						ImGui::PushID(static_cast<int>(i));
						const bool sel = (r.fullName == s_selected);
						if (ImGui::Selectable(r.fullName.c_str(), sel))
						{
							s_selected = r.fullName;
							std::snprintf(s_folderName, sizeof(s_folderName), "%s", r.name.c_str());
							s_formError.clear();
						}
						// Visibility and last update on the right, where a long
						// name does not push them out of sight.
						const std::string meta = std::string(r.isPrivate ? "private  " : "public  ") +
						                         r.updatedAt.substr(0, 10);
						ImGui::SameLine(ImGui::GetContentRegionMax().x -
						                ImGui::CalcTextSize(meta.c_str()).x);
						ImGui::TextDisabled("%s", meta.c_str());
						ImGui::PopID();
					}
					if (shown == 0) ImGui::TextDisabled("Nothing matches the search.");
					ImGui::EndChild();
					ImGui::EndDisabled();
				}
				for (const auto& r : repos)
					if (r.fullName == s_selected) { chosen = &r; break; }
			}

			// ── 3. Where → clone ─────────────────────────────────────────────
			if (chosen)
			{
				ImGui::Spacing();
				ImGui::SeparatorText("Clone into");
				if (chosen->defaultBranch.empty())
					ImGui::TextDisabled("This repository has no commits yet, so there is no "
					                    "project in it to open.");

				ImGui::BeginDisabled(cloning);
				ImGui::SetNextItemWidth(-80.0f);
				ImGui::InputText("##cloneparent", s_parentDir, sizeof(s_parentDir));
				ImGui::SameLine();
				if (EditorWidgets::button("Browse##clone", ImVec2(72.0f, 0.0f)))
					browseForParent(ctx);
				ImGui::SetNextItemWidth(-80.0f);
				ImGui::InputTextWithHint("##clonename", "Folder name",
				                         s_folderName, sizeof(s_folderName));
				ImGui::EndDisabled();

				const fs::path target = fs::path(s_parentDir) / s_folderName;
				ImGui::TextDisabled("%s", target.string().c_str());

				ImGui::Spacing();
				ImGui::BeginDisabled(cloning || s_parentDir[0] == '\0' || s_folderName[0] == '\0');
				if (EditorWidgets::primaryButton("Clone", ImVec2(140.0f, 0.0f)))
				{
					s_formError.clear();
					if (targetUsable(s_parentDir, s_folderName, s_formError))
					{
						// The token rides along to the credential helper and is
						// wiped here; a retry after a failure finds it in the
						// helper, or asks for it again.
						git->requestClone(chosen->cloneUrl, target, std::string(s_token));
						wipeToken();
						s_cloneStarted  = true;
						s_resultHandled = false;
						s_projectFile.clear();
					}
				}
				ImGui::EndDisabled();
				if (!s_formError.empty()) errorText(s_formError);
			}

			// ── 4. Progress and result ───────────────────────────────────────
			if (s_cloneStarted && cloning)
			{
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "%s",
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
					if (!git->cloneError().empty()) errorText(git->cloneError());
					else if (!git->cloneInfo().empty())
						ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "%s",
						                   git->cloneInfo().c_str());

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

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(git && git->cloneBusy());
		if (ImGui::Button("Close")) close = true;
		ImGui::EndDisabled();
	}

	if (close)
	{
		if (git && !git->cloneBusy()) git->finishClone();
		resetForm();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace GitCloneDialog
