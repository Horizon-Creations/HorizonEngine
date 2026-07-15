#!/usr/bin/env bash
# Make a flat directory of runtime binaries (the game / editor deploy) SELF-CONTAINED
# so it runs on a machine without the dev's system libraries (Homebrew lz4/zstd/openssl,
# a system CPython, …). Called from the HorizonGame POST_BUILD on macOS + Linux; the
# exporter copies the whole Game/ dir into an export, so fixing Game/ fixes exports too.
#
#   Usage: bundle_native_deps.sh <dir>
#
# macOS: BFS every Mach-O's non-system deps, copy them in next to the binaries, rewrite
#        the links to @rpath, and ad-hoc re-sign (install_name_tool invalidates the
#        signature; arm64 kills unsigned code on launch). The game exe already carries an
#        @executable_path rpath (set at build), and an export's dylibs sit next to the exe,
#        so @rpath resolves to the bundle dir in both the flat and .app layouts.
# Linux: copy the third-party sonames the engine links from system dirs (lz4/zstd, and
#        libcrypto when not statically mbedTLS). SDL3 is already deployed; the GL/X11/
#        Wayland/audio stack stays system (it's part of the desktop). The $ORIGIN rpath
#        (set globally at build) finds the copied libs next to the binaries.
set -eo pipefail

DIR="${1:?usage: bundle_native_deps.sh <dir>}"
[ -d "$DIR" ] || { echo "bundle_native_deps: '$DIR' is not a directory"; exit 1; }

# ── System-path predicate: deps we must NOT bundle (OS-provided) ────────────────
is_system_macho() {  # $1 = install path from otool
    case "$1" in
        @*|/usr/lib/*|/System/*|"") return 0 ;;   # already relative, or OS lib
        *) return 1 ;;
    esac
}

# Install paths a Mach-O links. Only tab-indented lines are dependency/id records;
# the "<file>:" and per-arch "<file> (architecture arm64):" header lines that otool
# emits for FAT binaries are not indented, so this skips them on every slice.
macho_deps() {  # $1 = mach-o file
    otool -L "$1" 2>/dev/null | grep '^[[:space:]]' | awk '{print $1}'
}

# Map a dependency install path to the basename we bundle it under. A framework
# binary (…/Python.framework/Versions/3.14/Python) has no extension; give it a
# .dylib name so it reads as a library AND the exporter's routeRuntime() sends it
# next to the exe (not into Resources/). Everything else keeps its basename.
bundled_name() {  # $1 = dep path -> echoes bundle basename
    case "$1" in
        */Python.framework/Versions/*/Python)
            printf 'libpython%s.dylib' \
                "$(printf '%s' "$1" | sed -E 's#.*/Versions/([0-9]+\.[0-9]+)/Python#\1#')" ;;
        *) basename "$1" ;;
    esac
}

bundle_macos() {
    local seen="|" items=() f dep name id
    # Seed the work list + seen-set with the binaries already in the dir.
    for f in "$DIR"/HorizonGame "$DIR"/HorizonEditor "$DIR"/*.dylib; do
        [ -f "$f" ] || continue
        items+=("$f"); seen="${seen}$(basename "$f")|"
    done

    # ── BFS: pull every transitive non-system dependency into the dir ───────────
    local i=0
    while [ "$i" -lt "${#items[@]}" ]; do
        local cur="${items[$i]}"; i=$((i + 1))
        while IFS= read -r dep; do
            is_system_macho "$dep" && continue
            name="$(bundled_name "$dep")"
            case "$seen" in *"|${name}|"*) continue ;; esac
            seen="${seen}${name}|"
            if [ -f "$dep" ]; then
                cp -L "$dep" "$DIR/$name"; chmod u+w "$DIR/$name"
                echo "    + $name  (from $dep)"
                items+=("$DIR/$name")
            else
                echo "    ! WARNING: dependency not on disk: $dep"
            fi
        done < <(macho_deps "$cur")
    done

    # ── Rewrite install names: own id + every reference to a now-bundled lib ─────
    for f in "$DIR"/HorizonGame "$DIR"/HorizonEditor "$DIR"/*.dylib; do
        [ -f "$f" ] || continue
        # Every bundled dylib gets a @rpath id (a no-op for engine dylibs that
        # already have it; fixes the copied third-party libs' absolute ids).
        case "$f" in
            *.dylib) install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null || true ;;
        esac
        while IFS= read -r dep; do
            is_system_macho "$dep" && continue
            name="$(bundled_name "$dep")"
            [ -f "$DIR/$name" ] && install_name_tool -change "$dep" "@rpath/$name" "$f" 2>/dev/null || true
        done < <(macho_deps "$f")
    done

    # ── Verify no stray absolute references remain ──────────────────────────────
    local bad=0
    for f in "$DIR"/HorizonGame "$DIR"/HorizonEditor "$DIR"/*.dylib; do
        [ -f "$f" ] || continue
        while IFS= read -r dep; do
            is_system_macho "$dep" && continue
            echo "    BAD in $(basename "$f"): $dep"; bad=1
        done < <(macho_deps "$f")
    done
    [ "$bad" -eq 0 ] || { echo "bundle_native_deps: absolute paths remain — aborting"; return 1; }

    # ── Ad-hoc re-sign (dylibs first, then the executable) ──────────────────────
    if command -v codesign >/dev/null 2>&1; then
        for f in "$DIR"/*.dylib; do [ -f "$f" ] && codesign --force --sign - "$f" 2>/dev/null || true; done
        for f in "$DIR"/HorizonGame "$DIR"/HorizonEditor; do [ -f "$f" ] && codesign --force --sign - "$f" 2>/dev/null || true; done
    fi

    # Relocate the Python C-extension modules' own non-system deps (only _ssl/_hashlib
    # need this — their OpenSSL libssl/libcrypto). Keep them INSIDE lib-dynload/ with
    # @loader_path so Python's OpenSSL stays a matched pair, isolated from the engine's
    # own (Homebrew) libcrypto at the bundle root.
    bundle_macos_dynload
}

# Make the .so in <DIR>/lib-dynload self-contained: pull each one's non-system deps
# into lib-dynload/ next to it and rewrite links to @loader_path (the dir of the
# loading .so). Only _ssl/_hashlib actually pull anything in (libssl + libcrypto); the
# other ~70 modules are libSystem-only and are left untouched.
bundle_macos_dynload() {
    local dyndir="$DIR/lib-dynload"
    [ -d "$dyndir" ] || return 0
    local seen="|" items=() f dep name touched=()

    for f in "$dyndir"/*.so "$dyndir"/*.dylib; do
        [ -f "$f" ] || continue
        items+=("$f"); seen="${seen}$(basename "$f")|"
    done

    # BFS: copy every transitive non-system dep into lib-dynload/.
    local i=0
    while [ "$i" -lt "${#items[@]}" ]; do
        local cur="${items[$i]}"; i=$((i + 1))
        while IFS= read -r dep; do
            is_system_macho "$dep" && continue
            name="$(basename "$dep")"
            case "$seen" in *"|${name}|"*) continue ;; esac
            seen="${seen}${name}|"
            if [ -f "$dep" ]; then
                cp -L "$dep" "$dyndir/$name"; chmod u+w "$dyndir/$name"
                echo "    + lib-dynload/$name  (from $dep)"
                items+=("$dyndir/$name"); touched+=("$dyndir/$name")
            else
                echo "    ! WARNING: lib-dynload dep not on disk: $dep"
            fi
        done < <(macho_deps "$cur")
    done

    # Rewrite ids (of copied dylibs) + every reference to a now-bundled sibling to
    # @loader_path, recording which files we modified so we re-sign exactly those.
    for f in "$dyndir"/*.so "$dyndir"/*.dylib; do
        [ -f "$f" ] || continue
        local changed=0
        case "$f" in
            *.dylib) install_name_tool -id "@loader_path/$(basename "$f")" "$f" 2>/dev/null && changed=1 || true ;;
        esac
        while IFS= read -r dep; do
            is_system_macho "$dep" && continue
            name="$(basename "$dep")"
            if [ -f "$dyndir/$name" ]; then
                install_name_tool -change "$dep" "@loader_path/$name" "$f" 2>/dev/null && changed=1 || true
            fi
        done < <(macho_deps "$f")
        [ "$changed" -eq 1 ] && touched+=("$f")
    done

    # Re-sign only the files we touched (install_name_tool invalidated their signature);
    # the untouched framework modules keep their valid signatures.
    if command -v codesign >/dev/null 2>&1; then
        for f in "${touched[@]}"; do [ -f "$f" ] && codesign --force --sign - "$f" 2>/dev/null || true; done
    fi
}

bundle_linux() {
    # Reference binary to read the resolved dep paths from.
    local ref=""
    for cand in "$DIR/libHorizonCore.so" "$DIR/HorizonGame" "$DIR/HorizonEditor"; do
        [ -f "$cand" ] && ref="$cand" && break
    done
    [ -n "$ref" ] || { echo "bundle_native_deps: no reference binary in $DIR"; return 0; }

    # Copy the specific third-party sonames the engine links from system dirs. We do
    # NOT sweep everything ldd reports — the GL/X11/Wayland/audio/glibc stack must stay
    # system. lz4/zstd are always linked (hpak); libcrypto only when not static mbedTLS.
    local want soname real
    for want in liblz4.so libzstd.so libcrypto.so; do
        # Match the actual versioned soname (liblz4.so.1, libcrypto.so.3, …) that ldd resolved.
        while IFS= read -r line; do
            soname="$(printf '%s' "$line" | awk '{print $1}')"
            real="$(printf '%s' "$line" | awk '{print $3}')"
            case "$soname" in
                ${want}*)
                    if [ -n "$real" ] && [ -f "$real" ] && [ ! -e "$DIR/$soname" ]; then
                        cp -L "$real" "$DIR/$soname"
                        echo "    + $soname  (from $real)"
                    fi ;;
            esac
        done < <(ldd "$ref" 2>/dev/null | grep -F "$want")
    done
}

echo "==> bundle_native_deps: making '$DIR' self-contained"
case "$(uname -s)" in
    Darwin) bundle_macos ;;
    Linux)  bundle_linux ;;
    *)      echo "    (no-op on $(uname -s))" ;;
esac
echo "==> bundle_native_deps: done"
