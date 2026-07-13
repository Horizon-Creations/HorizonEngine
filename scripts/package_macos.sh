#!/usr/bin/env bash
# Creates HorizonEditor.app bundle + HorizonEditor-<version>-macOS.dmg
#
# Usage (manual):  scripts/package_macos.sh [version]
# Usage (CMake):   cmake --build <build_dir> --target dmg
#
# Sources from:    out/deploy/Editor/   (populated by the normal build POST_BUILD)
# Output:          out/HorizonEditor-<version>-macOS.dmg
#
# Requirements: Xcode Command Line Tools (codesign, hdiutil, install_name_tool)
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="${1:-0.2.0}"

# Sky-themed release codename — single source of truth is CMakeLists.txt. Shown in
# the macOS About panel as "Version <VERSION> (<CODENAME>)", e.g. 0.2.0 (Sunrise).
CODENAME="${DMG_CODENAME:-$(grep -oE 'HE_VERSION_CODENAME "[^"]+"' "$SOURCE_DIR/CMakeLists.txt" 2>/dev/null | head -1 | sed -E 's/.*"([^"]+)".*/\1/')}"
[ -n "$CODENAME" ] || CODENAME="$VERSION"

APP_NAME="HorizonEditor"
DMG_NAME="${APP_NAME}-${VERSION}.dmg"

DEPLOY_DIR="$SOURCE_DIR/out/deploy/Editor"
STAGING="$SOURCE_DIR/out/dmg_staging"
APP_PATH="$STAGING/${APP_NAME}.app"
MACOS_PATH="$APP_PATH/Contents/MacOS"
FW_PATH="$APP_PATH/Contents/Frameworks"
RES_PATH="$APP_PATH/Contents/Resources"
BINARY="$MACOS_PATH/$APP_NAME"
DMG_OUT="$SOURCE_DIR/out/$DMG_NAME"

echo "======================================================================"
echo "  HorizonEngine macOS packager   v${VERSION}"
echo "======================================================================"
echo "  Source : $DEPLOY_DIR"
echo "  Output : $DMG_OUT"
echo ""

# ─── 1. Sanity check ──────────────────────────────────────────────────────────
if [ ! -f "$DEPLOY_DIR/$APP_NAME" ]; then
    echo "ERROR: $DEPLOY_DIR/$APP_NAME not found."
    echo "Run a full build first:  cmake --build <build_dir> --target HorizonEditor"
    exit 1
fi

# ─── 1b. DMG tooling (icon + styled installer window) ─────────────────────────
# A small Python venv (cached in out/.dmgvenv) provides Pillow (asset rendering)
# and dmgbuild (headless styled-DMG layout — no Finder/AppleScript, so it never
# triggers a TCC "control Finder" prompt). If this can't be set up (e.g. offline),
# FANCY=0 and we fall back to a plain DMG with no icon/background.
ASSETS_DIR="$SCRIPT_DIR/dmg_assets"
ICON_SRC="$SOURCE_DIR/EditorDeps/Images/HC_Logo.png"
# Procedural backdrop theme — keep this tracking the release codename in
# docs/version-codenames.md (0.2.0 "Sunrise" → sunrise).
DMG_THEME="${DMG_THEME:-sunrise}"           # twilight | midnight | sunrise
DMG_PHOTO="${DMG_PHOTO:-}"                   # optional: a screenshot backdrop (overrides DMG_THEME)
DMG_VENV="$SOURCE_DIR/out/.dmgvenv"
FANCY=1

if "$DMG_VENV/bin/python" -c "import dmgbuild, PIL" >/dev/null 2>&1; then
    DMG_PY="$DMG_VENV/bin/python"
elif python3 -m venv "$DMG_VENV" >/dev/null 2>&1 \
     && "$DMG_VENV/bin/pip" install --quiet --disable-pip-version-check dmgbuild Pillow >/dev/null 2>&1 \
     && "$DMG_VENV/bin/python" -c "import dmgbuild, PIL" >/dev/null 2>&1; then
    echo "--> Set up DMG tooling (dmgbuild + Pillow) in out/.dmgvenv"
    DMG_PY="$DMG_VENV/bin/python"
else
    echo "    WARNING: could not set up dmgbuild/Pillow — building a plain DMG."
    FANCY=0
fi
[ -f "$ICON_SRC" ] || { echo "    WARNING: $ICON_SRC missing — plain DMG."; FANCY=0; }

# ─── 2. Bundle skeleton ───────────────────────────────────────────────────────
echo "--> Creating bundle skeleton..."
rm -rf "$STAGING"
mkdir -p "$MACOS_PATH" "$FW_PATH" "$RES_PATH"

# ─── 3. Binary ────────────────────────────────────────────────────────────────
cp "$DEPLOY_DIR/$APP_NAME" "$BINARY"

# ─── 4. Engine dylibs: copy everything from the flat deploy ───────────────────
echo "--> Copying engine dylibs..."
for lib in "$DEPLOY_DIR/"*.dylib; do
    [ -f "$lib" ] || continue
    cp "$lib" "$FW_PATH/"
    echo "    $(basename "$lib")"
done

# ─── 5. BFS: find additional non-system deps (e.g. Homebrew liblz4) ──────────
# Engine dylibs already use @rpath/... internally, but may reference absolute
# Homebrew/local paths for third-party libs (e.g. liblz4).
echo "--> Scanning for additional dependencies..."
SEEN="|"
for lib in "$FW_PATH/"*.dylib; do
    [ -f "$lib" ] || continue
    SEEN="${SEEN}$(basename "$lib")|"
done

BFS_IDX=0
BFS_ITEMS=()
for lib in "$FW_PATH/"*.dylib; do
    [ -f "$lib" ] || continue
    BFS_ITEMS+=("$lib")
done

while [ "$BFS_IDX" -lt "${#BFS_ITEMS[@]}" ]; do
    CUR="${BFS_ITEMS[$BFS_IDX]}"
    BFS_IDX=$((BFS_IDX + 1))

    while IFS= read -r dep; do
        dep_name="$(basename "$dep")"
        case "$dep" in
            @*|/usr/lib/*|/System/*|"") continue ;;
        esac
        case "$SEEN" in *"|${dep_name}|"*) continue ;; esac
        SEEN="${SEEN}${dep_name}|"
        if [ -f "$dep" ]; then
            echo "    + ${dep_name}  (from ${dep})"
            cp -L "$dep" "$FW_PATH/"   # -L dereferences symlinks
            BFS_ITEMS+=("$FW_PATH/$dep_name")
        else
            echo "    ! WARNING: dependency not on disk: $dep"
        fi
    done < <(otool -L "$CUR" 2>/dev/null | tail -n +2 | awk '{print $1}')
done

# ─── 6. Resources (Fonts + Images) ────────────────────────────────────────────
# SDL_GetBasePath() returns Contents/Resources/ for a bundled .app.
echo "--> Copying resources to Contents/Resources/ ..."
EDITOR_DEPS="$SOURCE_DIR/EditorDeps"
[ -d "$EDITOR_DEPS/Fonts"  ] && cp -R "$EDITOR_DEPS/Fonts"  "$RES_PATH/"
[ -d "$EDITOR_DEPS/Images" ] && cp -R "$EDITOR_DEPS/Images" "$RES_PATH/"

# ─── 6b. App icon (.icns from the HC logo) ────────────────────────────────────
# Must land in Resources/ and be referenced in Info.plist BEFORE codesign so the
# signature covers it and the Finder/Dock icon sticks.
ICON_PLIST=""
if [ "$FANCY" -eq 1 ]; then
    echo "--> Generating app icon from HC logo..."
    if "$DMG_PY" "$ASSETS_DIR/gen_assets.py" icon --logo "$ICON_SRC" --out "$RES_PATH/AppIcon.icns"; then
        ICON_PLIST=$'\n    <key>CFBundleIconFile</key>\n    <string>AppIcon</string>'
    else
        echo "    WARNING: icon generation failed — bundle will have no custom icon."
    fi
fi

# ─── 7. Info.plist ────────────────────────────────────────────────────────────
echo "--> Writing Info.plist..."
cat > "$APP_PATH/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>HorizonEditor</string>
    <key>CFBundleIdentifier</key>
    <string>dev.horizoncreations.horizon-editor</string>
    <key>CFBundleName</key>
    <string>HorizonEditor</string>
    <key>CFBundleDisplayName</key>
    <string>Horizon Editor</string>
    <key>CFBundleVersion</key>
    <string>${CODENAME}</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>${ICON_PLIST}
</dict>
</plist>
PLIST

# ─── 8. Fix rpaths on the binary ──────────────────────────────────────────────
# The binary has absolute build-tree rpaths (e.g. /Users/.../cmake-build-release/src/HE_Core).
# Remove them all and replace with the bundle-relative path.
echo "--> Fixing rpaths..."
while IFS= read -r rpath; do
    install_name_tool -delete_rpath "$rpath" "$BINARY" 2>/dev/null || true
done < <(otool -l "$BINARY" | awk '/LC_RPATH/{f=1} f && /path /{print $2; f=0}')
install_name_tool -add_rpath "@executable_path/../Frameworks" "$BINARY"

# ─── 9. Fix dylib install names and cross-references ─────────────────────────
# Engine dylibs already use @rpath/... for their own install names and for
# references to other engine dylibs. Only third-party bundled libs (e.g. liblz4)
# need their install name changed from absolute to @rpath-relative.
for fw_lib in "$FW_PATH/"*.dylib; do
    [ -f "$fw_lib" ] || continue
    lib_name="$(basename "$fw_lib")"

    # Fix own install name if absolute (e.g. /opt/homebrew/... for liblz4)
    current_id="$(otool -D "$fw_lib" 2>/dev/null | tail -1)"
    case "$current_id" in
        @*) ;;  # already relative, leave it
        *)  install_name_tool -id "@rpath/$lib_name" "$fw_lib" ;;
    esac

    # Fix absolute references to other bundled libs (e.g. libHorizonCore → liblz4)
    while IFS= read -r dep; do
        dep_name="$(basename "$dep")"
        case "$dep" in
            @*|/usr/lib/*|/System/*|"") continue ;;
        esac
        if [ -f "$FW_PATH/$dep_name" ]; then
            install_name_tool -change "$dep" "@rpath/$dep_name" "$fw_lib" 2>/dev/null || true
        fi
    done < <(otool -L "$fw_lib" 2>/dev/null | tail -n +2 | awk '{print $1}')
done

# ─── 10. Verify: no stray absolute paths remain ───────────────────────────────
echo "--> Verifying linkage..."
VERIFY_FAIL=0
for f in "$BINARY" "$FW_PATH/"*.dylib; do
    [ -f "$f" ] || continue
    while IFS= read -r dep; do
        case "$dep" in
            @*|/usr/lib/*|/System/*|"") continue ;;
        esac
        echo "    BAD in $(basename "$f"): $dep"
        VERIFY_FAIL=1
    done < <(otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}')
done
if [ "$VERIFY_FAIL" -eq 1 ]; then
    echo "    WARNING: absolute paths remain — app may not run on other machines."
else
    echo "    OK — all references are system libs or @rpath-relative."
fi

# ─── 11. Code sign (ad-hoc) ───────────────────────────────────────────────────
# Ad-hoc signing lets the app run locally. For notarized public distribution,
# replace '-' with your Developer ID: "Developer ID Application: Name (TEAMID)"
# then run: xcrun notarytool submit out/HorizonEditor-*.dmg --wait
echo "--> Code-signing (ad-hoc)..."
if command -v codesign &>/dev/null; then
    for fw_lib in "$FW_PATH/"*.dylib; do
        [ -f "$fw_lib" ] || continue
        codesign --force --sign - "$fw_lib" 2>/dev/null || true
    done
    codesign --force --deep --sign - "$APP_PATH" 2>/dev/null && \
        echo "    Signed." || echo "    codesign failed (non-fatal for local use)."
else
    echo "    codesign not found, skipping."
fi

# ─── 12. Create DMG ───────────────────────────────────────────────────────────
mkdir -p "$SOURCE_DIR/out"
rm -f "$DMG_OUT"

if [ "$FANCY" -eq 1 ]; then
    PHOTO_ARG=()
    if [ -n "$DMG_PHOTO" ] && [ -f "$DMG_PHOTO" ]; then
        PHOTO_ARG=(--photo "$DMG_PHOTO")
        echo "--> Rendering DMG background (screenshot: $(basename "$DMG_PHOTO"))..."
    else
        echo "--> Rendering DMG background ('${DMG_THEME}' theme)..."
    fi
    WORK="$SOURCE_DIR/out/dmg_work"
    rm -rf "$WORK"; mkdir -p "$WORK"
    if "$DMG_PY" "$ASSETS_DIR/gen_assets.py" background \
            --logo "$ICON_SRC" --theme "$DMG_THEME" "${PHOTO_ARG[@]}" --out "$WORK/background.png" \
       && tiffutil -cathidpicheck "$WORK/background.png" "$WORK/background@2x.png" \
            -out "$WORK/background.tiff" >/dev/null 2>&1; then

        echo "--> Building styled DMG (dmgbuild)..."
        if "$DMG_VENV/bin/dmgbuild" \
                -s "$ASSETS_DIR/dmg_settings.py" \
                -D app="$APP_PATH" \
                -D background="$WORK/background.tiff" \
                -D volicon="$RES_PATH/AppIcon.icns" \
                "${APP_NAME} ${VERSION}" "$DMG_OUT"; then
            echo "    Styled DMG created."
        else
            echo "    WARNING: dmgbuild failed — falling back to a plain DMG."
            FANCY=0
        fi
    else
        echo "    WARNING: background render failed — falling back to a plain DMG."
        FANCY=0
    fi
fi

if [ "$FANCY" -eq 0 ]; then
    echo "--> Creating plain DMG..."
    ln -sf /Applications "$STAGING/Applications"
    hdiutil create \
        -volname "${APP_NAME} ${VERSION}" \
        -srcfolder "$STAGING" \
        -ov \
        -format UDZO \
        "$DMG_OUT"
fi

# ─── 12b. Custom Finder icon on the .dmg FILE ─────────────────────────────────
# dmgbuild sets the *mounted volume* icon, but not the icon Finder shows for the
# .dmg file itself. Embed the HC icon into the file's resource fork + set the
# "has custom icon" attribute so the download shows the logo.
ICNS_FILE="$RES_PATH/AppIcon.icns"
if [ -f "$ICNS_FILE" ] && [ -f "$DMG_OUT" ] \
   && xcrun -f Rez >/dev/null 2>&1 && xcrun -f DeRez >/dev/null 2>&1; then
    echo "--> Setting Finder icon on the DMG file..."
    TMPIC="$(mktemp -d)"
    cp "$ICNS_FILE" "$TMPIC/icon.icns"
    if sips -i "$TMPIC/icon.icns" >/dev/null 2>&1 \
       && xcrun DeRez -only icns "$TMPIC/icon.icns" > "$TMPIC/icon.rsrc" 2>/dev/null \
       && xcrun Rez -append "$TMPIC/icon.rsrc" -o "$DMG_OUT" 2>/dev/null; then
        xcrun SetFile -a C "$DMG_OUT"
        echo "    DMG file icon set."
    else
        echo "    WARNING: could not set DMG file icon (non-fatal)."
    fi
    rm -rf "$TMPIC"
fi

echo ""
echo "======================================================================"
echo "  Done: $DMG_OUT"
echo "======================================================================"
echo ""
echo "  NOTE: This DMG is ad-hoc signed. Users downloading it may see a"
echo "  Gatekeeper warning. They can bypass it with:"
echo "    sudo xattr -rd com.apple.quarantine HorizonEditor.app"
echo ""
echo "  For notarized public distribution:"
echo "    1. Re-sign with: codesign --sign \"Developer ID Application: ...\" ..."
echo "    2. Notarize:     xcrun notarytool submit $DMG_OUT --wait"
