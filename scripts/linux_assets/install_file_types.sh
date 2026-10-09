#!/bin/sh
# Registers the Horizon Editor as the application for .heproj project files on
# this Linux desktop, for the CURRENT USER (nothing is written outside
# $XDG_DATA_HOME, default ~/.local/share, and no root is needed).
#
#   FileTypes/install_file_types.sh              register this editor
#   FileTypes/install_file_types.sh --uninstall  take it away again
#
# Why this is a script and not a package: the Linux editor ships as a tarball
# you unpack anywhere, so the one thing a .desktop file has to say -- WHERE the
# editor is -- is only known here. Run it once after unpacking, and again if you
# move the folder (the entry holds the absolute path).
#
# What it installs, and what the desktop needs for a double-click to work:
#   mime/packages/horizon-editor.xml      names the type: *.heproj -> application/x-heproj
#   applications/horizon-editor.desktop   says the editor opens that type (MimeType=)
#   icons/hicolor/.../mimetypes/...png    the file icon in the file manager
#   icons/hicolor/.../apps/...png         the editor's own icon
# followed by update-mime-database, update-desktop-database and xdg-mime when
# they exist. The file manager then runs:  HorizonEditor <path-to-.heproj>
set -eu

usage() {
    echo "usage: $0 [--uninstall]" >&2
}

uninstall=0
case "${1:-}" in
    "")          ;;
    --uninstall) uninstall=1 ;;
    -h|--help)   usage; exit 0 ;;
    *)           usage; exit 2 ;;
esac

here=$(cd -P "$(dirname "$0")" && pwd -P)
editor_dir=$(dirname "$here")
exe="$editor_dir/HorizonEditor"

# The Desktop Entry spec wants an absolute XDG_DATA_HOME; a relative one is ignored.
data="${XDG_DATA_HOME:-}"
case "$data" in
    /*) ;;
    *)  data="${HOME:?HOME is not set}/.local/share" ;;
esac

desktop_file="$data/applications/horizon-editor.desktop"
mime_file="$data/mime/packages/horizon-editor.xml"
mime_icon="$data/icons/hicolor/256x256/mimetypes/application-x-heproj.png"
app_icon="$data/icons/hicolor/256x256/apps/horizon-editor.png"

# Run a helper if the system has it. None of them is required for the files to be
# in place, and a minimal desktop without one should not make the install fail.
refresh() {
    label=$1
    shift
    if command -v "$1" >/dev/null 2>&1; then
        "$@" >/dev/null 2>&1 || echo "note: '$label' reported a problem; log out and in to refresh the desktop" >&2
    else
        echo "note: $1 not found, so the $label step was skipped" >&2
    fi
}

refresh_all() {
    refresh "mime database" update-mime-database "$data/mime"
    refresh "desktop database" update-desktop-database "$data/applications"
    refresh "icon cache" gtk-update-icon-cache -q -t -f "$data/icons/hicolor"
}

if [ "$uninstall" -eq 1 ]; then
    rm -f "$desktop_file" "$mime_file" "$mime_icon" "$app_icon"
    refresh_all
    echo "Removed the .heproj file type registration from $data"
    echo "(an 'xdg-mime default' entry in mimeapps.list may remain; it points at a file that is gone.)"
    exit 0
fi

[ -x "$exe" ] || { echo "error: $exe not found or not executable (run this from the unpacked editor)" >&2; exit 1; }
for f in horizon-editor.desktop horizon-editor.xml application-x-heproj.png horizon-editor.png; do
    [ -f "$here/$f" ] || { echo "error: $here/$f is missing" >&2; exit 1; }
done

# The Exec line double-quotes the path (it may hold spaces). Inside the quotes the
# spec makes four characters special; a path with one of them is refused rather than
# registered half-working. '%' is not refused, it is doubled -- see below.
case "$exe" in
    *\"*|*\`*|*\$*|*\\*)
        echo "error: the editor's path contains one of  \" \` \$ \\  which a .desktop Exec line cannot carry: $exe" >&2
        exit 1 ;;
esac

mkdir -p "$data/applications" "$data/mime/packages" \
         "$data/icons/hicolor/256x256/mimetypes" "$data/icons/hicolor/256x256/apps"

# The template is a valid .desktop file on its own; only the lines that depend on
# where the editor lives are replaced, so nothing in the path is ever interpreted
# as sed or shell syntax.
exe_field=$(printf '%s' "$exe" | sed 's/%/%%/g')
tmp="$desktop_file.tmp.$$"
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        Exec=*)    printf 'Exec="%s" %%f\n' "$exe_field" ;;
        TryExec=*) printf 'TryExec=%s\n' "$exe" ;;
        *)         printf '%s\n' "$line" ;;
    esac
done < "$here/horizon-editor.desktop" > "$tmp"
mv "$tmp" "$desktop_file"

cp "$here/horizon-editor.xml" "$mime_file"
cp "$here/application-x-heproj.png" "$mime_icon"
cp "$here/horizon-editor.png" "$app_icon"

refresh_all
# Make the editor THE handler rather than one of several, so a double-click does not
# ask. Without xdg-utils the mimeinfo cache above still lists it as a handler.
refresh "default application" xdg-mime default horizon-editor.desktop application/x-heproj

echo "Registered .heproj files with $exe"
echo "  desktop entry: $desktop_file"
echo "  mime type:     $mime_file"
