# dmgbuild settings for the HorizonEditor installer DMG.
#
# Invoked by scripts/package_macos.sh, e.g.:
#   dmgbuild -s dmg_settings.py \
#            -D app=<…/HorizonEditor.app> \
#            -D background=<…/background.tiff> \
#            -D volicon=<…/AppIcon.icns> \
#            "HorizonEditor 1.0"  out/HorizonEditor-1.0-macOS.dmg
#
# Window/icon geometry must match scripts/dmg_assets/gen_assets.py (WIN_*, *_XY).
import os.path

app = defines.get("app")
appname = os.path.basename(app)

# ── Disk image ──────────────────────────────────────────────────────────────────
format = "UDZO"            # zlib-compressed, read-only (same as the old plain DMG)
compression_level = 9
filesystem = "HFS+"

files = [app]
symlinks = {"Applications": "/Applications"}

# Volume icon → the mounted disk shows the HC logo squircle.
icon = defines.get("volicon")

# ── Finder window ───────────────────────────────────────────────────────────────
background = defines.get("background")
# Height = background height (400) + Finder title-bar (~28) so the full 640x400
# background image is revealed in the content area rather than clipped at the bottom.
window_rect = ((420, 180), (640, 428))   # ((x, y) on screen, (width, height))
default_view = "icon-view"
show_icon_preview = False
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False

# ── Icon layout ─────────────────────────────────────────────────────────────────
icon_size = 128
text_size = 13
icon_locations = {
    appname: (168, 256),
    "Applications": (472, 256),
}
