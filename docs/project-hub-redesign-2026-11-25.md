# Project Hub redesign (2026-11-25)

The hub was three plain columns: Create Project (template list as text), Recent Projects
(name + path rows), Open Project (Browse / Clone). It is now two columns:

* **Projects** (left, 42 %): Browse .heproj... and Clone from GitHub... on one row, then
  the recent projects as cards. A card shows the picture the project was last closed on,
  its name and its folder (`~`-shortened). Without a picture it shows the brand's horizon
  placeholder, and a missing project shows "Not found" and does not open.
* **New Project** (right, 58 %): one tile per template with a drawn picture, in two
  groups (Games & Scenes, Applications), then the form (language, name + folder side by
  side, Create). The column scrolls when the window is short; the tiles never do.

The pictures on the template tiles are drawn with ImDrawList (`drawTemplateArt`), not
shipped as image files. The App templates are layouts, so a small wireframe of the layout
is the picture, and drawing needs no files for the packager.

## Project thumbnail

`EditorApplication::captureProjectThumbnail` reads the viewport (`IRenderer::CaptureViewport`),
cover-crops it to 16:9, box-filters it to 480×270 and writes
`<project>/Saved/ProjectThumbnail.png` (`ProjectThumbnail.h`). It is taken in two places:
`EditorUI::endProjectSession` (Close Project, every project switch) and the first line of
`OnShutdown` (every quit). It is skipped for headless dump runs (`m_dumpPath`) and for a
flat frame (an application project has no world). `Saved/` is already in the `.gitignore`
the editor writes (`RepoConfig::gitignoreText`), so the picture is not committed.

## Handoff

- **Topic / step:** Project Hub redesign, single step (from a side session, not the Hive)
- **Branch / commit:** release/0.8.0 (see git log, "Project Hub: picture cards …")
- **Done:** Redesigned hub with merged Projects section and template tiles. Thumbnail captured on close/quit and shown on the cards. File ▸ New Project popup uses the same tiles (720 px wide).
- **Files touched:** `src/HE_Editor/ProjectHubPanel.cpp` (layout, `drawTemplateGrid`, `drawTemplateArt`, `thumbnailFor`); `ProjectHubPanel.h` (`drawTemplateGrid`, `releaseTextures`, `setTextureHooks`); `ProjectThumbnail.{h,cpp}` (crop/scale/save); `EditorApplication.cpp` (`captureProjectThumbnail`, OnShutdown); `EditorUI.cpp` (`endProjectSession`, `drawNewProjectPopup`); `tests/test_project_hub_ui.cpp`.
- **Verified:**
  - `he_tests --source-file='*test_project_hub_ui.cpp'`: 3 cases, 33 assertions pass. They cover crop/PNG round trip, the flat frame not being saved, card click → open, missing card → no open, Clone, tile → template, and all tiles visible at 1000×620.
  - Headless shots at 1280×800 and 1000×620 (`HE_UI_DUMP_DIR`) were looked at: real thumbnail, placeholder and "Not found" card all draw correctly.
  - The real editor, headless (`HE_EXIT_AFTER_FRAMES=120`, private `HE_CONFIG_DIR` + `HOME`, copy of a project), wrote `Saved/ProjectThumbnail.png` on the clean exit; the picture showed the viewport.
  - `editor_help_audit.py --check` green.
- **Not verified:**
  - The hub in a visible window with a picture uploaded through a real backend; the shots use the software raster's texture registry.
  - Close Project → hub in the GUI.
  - Vulkan/D3D (not runnable on this Mac).
  - The committed state alone: it was built together with another session's uncommitted work in `EditorApplication.cpp` and `EditorUI.cpp`.
- **Decisions taken:**
  - Projects on the left: returning users open more often than they create.
  - Template pictures are drawn, not image files.
  - The three existing help labels are kept verbatim.
  - A Play-mode frame counts as "what was visible".
- **Decisions suggested, not taken:** A "Show in Finder" item on the card's context menu; a sort/search over projects (the list is at most ten).
- **Pending:** The manual (`EditorDeps/Docs/he-docs.json`, built from the website docs) still says "a list of known projects plus a New Project form". It should mention the cards and their picture.
- **Traps:**
  - A `config.json` in the working directory wins over a private `HOME`. The repo root has one (gitignored), so running the editor from the repo root opens that config's project and rewrites the file on exit. Run from a scratch dir with `HE_CONFIG_DIR` set.
  - Vulkan's ImGui texture pool (64) is never given back, so the hub creates a card texture only when the file's mtime changes.
- **Next steps:** 1. Look at the hub once in the GUI (open a project, Close Project). 2. Update the website manual's Project Hub section.
