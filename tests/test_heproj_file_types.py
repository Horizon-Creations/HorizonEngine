#!/usr/bin/env python3
"""Tests for the .heproj file type registration of the Windows and Linux packages.

Double-clicking a project only opens the editor if the OS has been told that .heproj
is the editor's. macOS says so in Info.plist (scripts/package_macos.sh). Windows and
Linux ship the editor as a ZIP / tarball with no installer, so the registration is a
set of files inside the package, FileTypes/, that the user can run once
(scripts/windows_assets, scripts/linux_assets, wired in src/HE_Editor/CMakeLists.txt).
Since topic 161 step 5 the editor also registers .heproj for itself at startup
(src/HE_Editor/HeprojRegistration.*), per user and without administrator rights; the
scripts stay as the fallback.

What this proves, without a Windows machine and without a file manager:
  * all three platforms use ONE name for the type (the macOS UTI, the Windows ProgID,
    the Linux MIME type), because a disagreement is how a file opens in the wrong
    application;
  * the files are well formed: the MIME XML parses, the .desktop file passes
    desktop-file-validate, the icons are real ICO / PNG of the right size;
  * the Linux installer, run for real against a throwaway XDG_DATA_HOME in a path full
    of spaces and parentheses, writes an Exec line that quotes that path, builds a MIME
    database that maps *.heproj to application/x-heproj, and (on Linux with glib) hands
    the project path to the editor when the desktop entry is launched;
  * with --package DIR, the deployed package really contains those files;
  * the editor's self-registration names the type like the scripts do (its source is
    read as text), and on Linux the PACKAGED binary, run as
    `HorizonEditor --register-file-types` against a throwaway HOME and XDG directories,
    writes the very files the installer script writes (byte for byte), reports
    registered / updated / already-registered (and then touches nothing at all), repairs
    a missing or stale file, follows the XDG fallbacks of the script, and leaves a
    foreign default handler for .heproj alone (class LinuxSelfRegistration). That class
    needs the BUILT binary, so it runs only with --package on Linux and is skipped
    everywhere else, the Mac included.

What it cannot prove: that Windows Explorer or a Linux file manager reacts to a
double-click. The Windows registry side is exercised on the Windows runner by
scripts/windows_assets/check_heproj_registration.ps1; the double-click itself stays a
hardware check.

Run directly (python3 tests/test_heproj_file_types.py [--package DIR]) or via ctest
(heproj_file_types).
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN_DIR = os.path.join(REPO_ROOT, "scripts", "windows_assets")
LIN_DIR = os.path.join(REPO_ROOT, "scripts", "linux_assets")

PROG_ID = "dev.horizoncreations.heproj"
MIME = "application/x-heproj"
TYPE_NAME = "Horizon Engine Project"

# What each platform's package carries in FileTypes/. The CMake test below holds
# src/HE_Editor/CMakeLists.txt to exactly these lists.
WINDOWS_FILES = ["register_heproj.cmd", "unregister_heproj.cmd", "register_heproj.ps1", "heproj.ico"]
LINUX_FILES = ["install_file_types.sh", "horizon-editor.desktop", "horizon-editor.xml",
               "application-x-heproj.png", "horizon-editor.png"]

def _take_package_arg():
    """--package DIR is ours, not unittest's: read it before the classes are built (the
    skipIf below is decided at import) and remove it from argv."""
    if "--package" not in sys.argv:
        return None
    i = sys.argv.index("--package")
    if i + 1 >= len(sys.argv):
        sys.exit("--package needs a directory")
    path = os.path.abspath(sys.argv[i + 1])
    del sys.argv[i:i + 2]
    return path


PACKAGE_DIR = _take_package_arg()


def read_text(*parts):
    with open(os.path.join(*parts), "r", encoding="utf-8", newline=None) as f:
        return f.read()


def read_bytes(*parts):
    with open(os.path.join(*parts), "rb") as f:
        return f.read()


def parse_desktop(text):
    """The keys of the [Desktop Entry] group, comments and blank lines skipped."""
    keys = {}
    in_group = False
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("["):
            in_group = line == "[Desktop Entry]"
            continue
        if in_group and "=" in line:
            k, v = line.split("=", 1)
            keys[k] = v
    return keys


class OneNameEverywhere(unittest.TestCase):
    """The three platforms must say the same thing about what a .heproj is."""

    def test_macos_plist_uses_the_same_identifiers(self):
        script = read_text(REPO_ROOT, "scripts", "package_macos.sh")
        self.assertIn(PROG_ID, script)
        self.assertIn(MIME, script)
        self.assertIn("<string>heproj</string>", script)
        self.assertIn(TYPE_NAME, script)

    def test_windows_registration_names_the_type_the_same_way(self):
        ps1 = read_text(WIN_DIR, "register_heproj.ps1")
        self.assertIn("'%s'" % PROG_ID, ps1)
        self.assertIn("'%s'" % MIME, ps1)
        self.assertIn("'%s'" % TYPE_NAME, ps1)

    def test_linux_mime_xml_names_the_type_the_same_way(self):
        root = ET.fromstring(read_bytes(LIN_DIR, "horizon-editor.xml"))
        ns = "{http://www.freedesktop.org/standards/shared-mime-info}"
        types = root.findall(ns + "mime-type")
        self.assertEqual(len(types), 1)
        t = types[0]
        self.assertEqual(t.get("type"), MIME)
        self.assertEqual(t.find(ns + "comment").text, TYPE_NAME)
        self.assertEqual([g.get("pattern") for g in t.findall(ns + "glob")], ["*.heproj"])
        # A project file is JSON, as the macOS UTI says by conforming to public.json.
        self.assertEqual([s.get("type") for s in t.findall(ns + "sub-class-of")], ["application/json"])

    def test_linux_desktop_entry_claims_that_type(self):
        keys = parse_desktop(read_text(LIN_DIR, "horizon-editor.desktop"))
        self.assertEqual(keys["Type"], "Application")
        self.assertEqual(keys["MimeType"], MIME + ";")
        self.assertEqual(keys["Terminal"], "false")
        self.assertEqual(keys["Icon"], "horizon-editor")
        # %f, one project per process: %F would start ONE editor that ignores all but
        # the first project of a selection (ProjectLaunchOpen takes the first valid one).
        self.assertTrue(keys["Exec"].endswith(" %f"), keys["Exec"])
        self.assertNotIn("%F", keys["Exec"])
        self.assertIn("Development", keys["Categories"])

    def test_the_editors_self_registration_names_the_type_the_same_way(self):
        """The C++ that registers .heproj at editor startup (topic 161 step 5) must say
        what the scripts and the plist say; the names are literals in its source."""
        parts = []
        for name in ("HeprojRegistration.h", "HeprojRegistration.cpp"):
            path = os.path.join(REPO_ROOT, "src", "HE_Editor", name)
            if not os.path.isfile(path):
                self.skipTest("src/HE_Editor/%s does not exist yet (topic 161 step 5 is not in)" % name)
            parts.append(read_text(path))
        source = "\n".join(parts)
        for needle in (PROG_ID, MIME, TYPE_NAME, "horizon-editor.desktop"):
            self.assertIn(needle, source)


class WindowsFiles(unittest.TestCase):
    def test_registration_is_per_user_and_quotes_the_path_and_the_argument(self):
        ps1 = read_text(WIN_DIR, "register_heproj.ps1")
        # HKCU only: nothing here may need an administrator or touch the machine.
        self.assertIn("[Microsoft.Win32.Registry]::CurrentUser", ps1)
        self.assertNotIn("LocalMachine", ps1)
        self.assertNotIn("HKLM", ps1)
        # The open command is "<exe>" "%1": both quoted, so a path with spaces and a
        # project path with spaces each arrive as ONE argument.
        self.assertIn("""'"{0}" "%1"' -f $exe""", ps1)
        self.assertIn("shell\\open\\command", ps1)
        self.assertIn("DefaultIcon", ps1)
        self.assertIn("OpenWithProgids", ps1)
        self.assertIn("SHChangeNotify", ps1)
        self.assertIn("heproj.ico", ps1)
        # PowerShell 5.1 reads a BOM-less file as the ANSI code page: stay ASCII.
        ps1.encode("ascii")

    def test_unregister_only_removes_what_is_ours(self):
        ps1 = read_text(WIN_DIR, "register_heproj.ps1")
        self.assertIn("param([switch]$Unregister)", ps1)
        # The extension key is deleted only when its default is our ProgID.
        self.assertIn("if ($owner -eq $progId)", ps1)

    def test_launchers_run_the_script_without_the_execution_policy_in_the_way(self):
        reg = read_text(WIN_DIR, "register_heproj.cmd")
        unreg = read_text(WIN_DIR, "unregister_heproj.cmd")
        for text in (reg, unreg):
            self.assertIn("-ExecutionPolicy Bypass", text)
            self.assertIn('"%~dp0register_heproj.ps1"', text)
            self.assertIn('/i not "%~1"=="/quiet"', text)
            self.assertIn("exit /b %RC%", text)
        self.assertNotIn("-Unregister", reg)
        self.assertIn("-Unregister", unreg)

    def test_windows_scripts_get_crlf_from_git(self):
        attrs = read_text(REPO_ROOT, ".gitattributes")
        self.assertIn("scripts/windows_assets/*.cmd text eol=crlf", attrs)
        self.assertIn("scripts/windows_assets/*.ps1 text eol=crlf", attrs)

    def test_icon_is_a_real_ico_with_the_sizes_explorer_asks_for(self):
        data = read_bytes(WIN_DIR, "heproj.ico")
        reserved, kind, count = struct.unpack_from("<HHH", data, 0)
        self.assertEqual((reserved, kind), (0, 1))
        sizes = set()
        for i in range(count):
            w, h, _c, _r, _planes, _bpp, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
            self.assertLessEqual(offset + size, len(data))
            sizes.add(w or 256)           # a width byte of 0 means 256
        self.assertTrue({16, 32, 48, 256} <= sizes, sizes)


class LinuxFiles(unittest.TestCase):
    def test_png_icons_are_256_square(self):
        for name in ("application-x-heproj.png", "horizon-editor.png"):
            data = read_bytes(LIN_DIR, name)
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n", name)
            w, h = struct.unpack(">II", data[16:24])
            self.assertEqual((w, h), (256, 256), name)

    def test_installer_is_posix_sh_with_unix_line_endings(self):
        data = read_bytes(LIN_DIR, "install_file_types.sh")
        self.assertTrue(data.startswith(b"#!/bin/sh\n"))
        self.assertNotIn(b"\r", data, "a CR in the script breaks /bin/sh on the shebang line")
        if os.name == "posix":
            self.assertTrue(os.access(os.path.join(LIN_DIR, "install_file_types.sh"), os.X_OK))
        attrs = read_text(REPO_ROOT, ".gitattributes")
        self.assertIn("scripts/linux_assets/*.sh text eol=lf", attrs)

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-validate not installed")
    def test_the_template_passes_desktop_file_validate(self):
        r = subprocess.run(["desktop-file-validate", os.path.join(LIN_DIR, "horizon-editor.desktop")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class PackageWiring(unittest.TestCase):
    def test_cmake_ships_exactly_these_files(self):
        cmake = read_text(REPO_ROOT, "src", "HE_Editor", "CMakeLists.txt")
        lists = re.findall(r"set\(HE_FILETYPES_FILES\s+([^)]*)\)", cmake)
        self.assertEqual(len(lists), 2, "one list for Windows, one for Linux")
        shipped = [l.split() for l in lists]
        self.assertEqual(shipped, [WINDOWS_FILES, LINUX_FILES])
        for name in WINDOWS_FILES:
            self.assertTrue(os.path.isfile(os.path.join(WIN_DIR, name)), name)
        for name in LINUX_FILES:
            self.assertTrue(os.path.isfile(os.path.join(LIN_DIR, name)), name)
        # (check_heproj_registration.ps1 is a test aid and is not in either list.)
        # macOS carries its registration in the plist; the FileTypes block is not for it.
        self.assertIn("elseif(UNIX AND NOT APPLE)", cmake)

    @unittest.skipIf(PACKAGE_DIR is None, "no --package DIR given")
    def test_the_deployed_package_carries_the_files(self):
        pkg = PACKAGE_DIR
        if os.path.isfile(os.path.join(pkg, "HorizonEditor.exe")):
            source, names = WIN_DIR, WINDOWS_FILES
        elif os.path.isfile(os.path.join(pkg, "HorizonEditor")):
            source, names = LIN_DIR, LINUX_FILES
        else:
            self.fail("neither HorizonEditor.exe nor HorizonEditor in %s: not an editor package" % pkg)
        for name in names:
            deployed = os.path.join(pkg, "FileTypes", name)
            self.assertTrue(os.path.isfile(deployed), "missing from the package: FileTypes/" + name)
            # Compare bytes, not text: the checkout's line endings are part of the file.
            self.assertEqual(read_bytes(deployed), read_bytes(source, name), name)
        if os.name == "posix" and source == LIN_DIR:
            self.assertTrue(os.access(os.path.join(pkg, "FileTypes", "install_file_types.sh"), os.X_OK),
                            "install_file_types.sh lost its executable bit on the way into the package")
        self.assertFalse(os.path.exists(os.path.join(pkg, "FileTypes", "check_heproj_registration.ps1")))


@unittest.skipUnless(os.name == "posix" and shutil.which("sh"), "the installer is a POSIX shell script")
class LinuxInstaller(unittest.TestCase):
    """Runs install_file_types.sh for real, into a throwaway XDG_DATA_HOME, from an
    editor folder whose name has a space, parentheses and a quote-free but awkward shape:
    the places a hand-rolled .desktop Exec line goes wrong."""

    def setUp(self):
        self.tmp = os.path.realpath(tempfile.mkdtemp(prefix="he heproj "))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.data = os.path.join(self.tmp, "data home")
        self.args_file = os.path.join(self.tmp, "args.txt")
        self.env = dict(os.environ)
        self.env.update({
            "HOME": os.path.join(self.tmp, "home"),
            "XDG_DATA_HOME": self.data,
            "XDG_CONFIG_HOME": os.path.join(self.tmp, "config"),
            "XDG_DATA_DIRS": os.pathsep.join([self.data, "/usr/local/share", "/usr/share"]),
        })
        os.makedirs(self.env["HOME"])

    def make_editor(self, dirname):
        """An editor folder with the real FileTypes/ and a stub HorizonEditor that
        records the arguments it was started with."""
        editor = os.path.join(self.tmp, dirname)
        shutil.copytree(LIN_DIR, os.path.join(editor, "FileTypes"),
                        ignore=shutil.ignore_patterns("check_*"))
        stub = os.path.join(editor, "HorizonEditor")
        with open(stub, "w") as f:
            f.write("#!/bin/sh\nprintf '%%s\\n' \"$@\" > '%s'\n" % self.args_file)
        os.chmod(stub, 0o755)
        return editor

    def run_installer(self, editor, *args):
        return subprocess.run(["sh", os.path.join(editor, "FileTypes", "install_file_types.sh")] + list(args),
                              env=self.env, capture_output=True, text=True)

    def desktop_file(self):
        return os.path.join(self.data, "applications", "horizon-editor.desktop")

    def test_install_writes_an_entry_that_points_at_this_editor_and_quotes_the_path(self):
        editor = self.make_editor("Horizon Editor (x86)")
        r = self.run_installer(editor)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        text = read_text(self.desktop_file())
        keys = parse_desktop(text)
        exe = os.path.join(editor, "HorizonEditor")
        self.assertEqual(keys["Exec"], '"%s" %%f' % exe)
        self.assertEqual(keys["TryExec"], exe)
        # Everything else in the entry is the template, untouched.
        self.assertEqual(keys["MimeType"], MIME + ";")
        self.assertEqual(keys["Icon"], "horizon-editor")
        for rel in ("mime/packages/horizon-editor.xml",
                    "icons/hicolor/256x256/mimetypes/application-x-heproj.png",
                    "icons/hicolor/256x256/apps/horizon-editor.png"):
            self.assertTrue(os.path.isfile(os.path.join(self.data, rel)), rel)

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-validate not installed")
    def test_the_generated_entry_passes_desktop_file_validate(self):
        editor = self.make_editor("Horizon Editor (x86)")
        self.assertEqual(self.run_installer(editor).returncode, 0)
        r = subprocess.run(["desktop-file-validate", self.desktop_file()], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(shutil.which("update-mime-database"), "update-mime-database not installed")
    def test_the_mime_database_maps_the_extension_to_the_type(self):
        editor = self.make_editor("editor")
        r = self.run_installer(editor)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(MIME + ":*.heproj", read_text(self.data, "mime", "globs2"))
        self.assertIn(MIME + " application/json", read_text(self.data, "mime", "subclasses"))
        # update-desktop-database lists the entry as a handler of the type.
        if shutil.which("update-desktop-database"):
            self.assertIn(MIME + "=horizon-editor.desktop;", read_text(self.data, "applications", "mimeinfo.cache"))

    @unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("gio"),
                         "the desktop's own resolver (glib on Linux) is needed")
    def test_the_desktop_resolves_a_project_file_and_hands_it_to_the_editor(self):
        editor = self.make_editor("Horizon Editor (x86)")
        self.assertEqual(self.run_installer(editor).returncode, 0)
        project = os.path.join(self.tmp, "my project (1).heproj")
        with open(project, "w") as f:
            f.write("{}")
        # A hung glib call must not eat a ctest slot: every one of them has a timeout.
        info = subprocess.run(["gio", "info", "-a", "standard::content-type", project],
                              env=self.env, capture_output=True, text=True, timeout=30)
        self.assertIn("standard::content-type: " + MIME, info.stdout, info.stdout + info.stderr)
        handlers = subprocess.run(["gio", "mime", MIME], env=self.env, capture_output=True, text=True,
                                  timeout=30)
        self.assertIn("horizon-editor.desktop", handlers.stdout, handlers.stdout + handlers.stderr)
        # What a file manager does on a double-click: launch the entry with the file.
        launch = subprocess.run(["gio", "launch", self.desktop_file(), project],
                                env=self.env, capture_output=True, text=True, timeout=30)
        for _ in range(100):
            if os.path.exists(self.args_file) and read_text(self.args_file).strip():
                break
            time.sleep(0.1)
        else:
            self.skipTest("gio launch started nothing here (%s %s)" % (launch.returncode, launch.stderr.strip()))
        # One argument, the project, in one piece: a path with spaces and parentheses.
        self.assertEqual(read_text(self.args_file).splitlines(), [project])

    def test_a_percent_in_the_path_is_doubled_for_the_exec_field_codes(self):
        editor = self.make_editor("100% editor")
        r = self.run_installer(editor)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        exec_line = parse_desktop(read_text(self.desktop_file()))["Exec"]
        self.assertEqual(exec_line, '"%s" %%f' % os.path.join(editor, "HorizonEditor").replace("%", "%%"))

    def test_a_path_a_desktop_entry_cannot_carry_is_refused_not_half_registered(self):
        editor = self.make_editor('quote"editor')
        r = self.run_installer(editor)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("cannot carry", r.stderr)
        self.assertFalse(os.path.exists(self.desktop_file()))

    def test_an_installer_run_outside_an_editor_folder_says_so(self):
        editor = self.make_editor("editor")
        os.remove(os.path.join(editor, "HorizonEditor"))
        r = self.run_installer(editor)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("HorizonEditor", r.stderr)
        self.assertFalse(os.path.exists(self.desktop_file()))

    def test_uninstall_removes_what_install_wrote_and_is_repeatable(self):
        editor = self.make_editor("editor")
        self.assertEqual(self.run_installer(editor).returncode, 0)
        for _ in range(2):
            r = self.run_installer(editor, "--uninstall")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(self.desktop_file()))
        self.assertFalse(os.path.exists(os.path.join(self.data, "mime", "packages", "horizon-editor.xml")))
        self.assertFalse(os.path.exists(os.path.join(self.data, "icons", "hicolor", "256x256", "apps",
                                                     "horizon-editor.png")))

    def test_an_unknown_option_is_a_usage_error(self):
        editor = self.make_editor("editor")
        r = self.run_installer(editor, "--frobnicate")
        self.assertEqual(r.returncode, 2)
        self.assertFalse(os.path.exists(self.desktop_file()))


# -- the editor registers .heproj for itself (topic 161 step 5) ----------------------

# The four files the registration puts into the XDG data home (paths relative to it,
# "/" separated) and the FileTypes/ asset each one is made from.
DESKTOP_REL = "applications/horizon-editor.desktop"
MIME_XML_REL = "mime/packages/horizon-editor.xml"
MIME_ICON_REL = "icons/hicolor/256x256/mimetypes/application-x-heproj.png"
APP_ICON_REL = "icons/hicolor/256x256/apps/horizon-editor.png"
REGISTERED_FILES = [(DESKTOP_REL, "horizon-editor.desktop"),
                    (MIME_XML_REL, "horizon-editor.xml"),
                    (MIME_ICON_REL, "application-x-heproj.png"),
                    (APP_ICON_REL, "horizon-editor.png")]

# A mimeapps.list in which the user has chosen ANOTHER application for the type.
FOREIGN_DEFAULT = "[Default Applications]\napplication/x-heproj=other.desktop;\n"

PACKAGED_EDITOR = os.path.join(PACKAGE_DIR, "HorizonEditor") if PACKAGE_DIR else None


def _self_registration_skip_reason():
    """Why LinuxSelfRegistration cannot run here, or None when it can. Decided at import,
    like PACKAGE_DIR itself: without the built binary there is nothing to run."""
    if not sys.platform.startswith("linux"):
        return "the self-registration tests write the Linux desktop files (this is %s)" % sys.platform
    if PACKAGE_DIR is None:
        return "no --package DIR given: these tests run the packaged HorizonEditor"
    if not (os.path.isfile(PACKAGED_EDITOR) and os.access(PACKAGED_EDITOR, os.X_OK)):
        return "no executable HorizonEditor in %s" % PACKAGE_DIR
    return None


_SELF_REGISTRATION_SKIP = _self_registration_skip_reason()


def expected_desktop_text(template, exe):
    """What install_file_types.sh makes of horizon-editor.desktop for an editor at `exe`:
    the Exec and TryExec lines replaced, every other line (comments too) as it was, each
    line ended by exactly one newline."""
    lines = template.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    out = []
    for line in lines:
        if line.startswith("Exec="):
            out.append('Exec="%s" %%f' % exe.replace("%", "%%"))
        elif line.startswith("TryExec="):
            out.append("TryExec=" + exe)
        else:
            out.append(line)
    return "\n".join(out) + "\n"


def tree_state(root):
    """Everything below `root` as {relative path: ("dir",) or ("file", size, mtime_ns)};
    {} when root does not exist. Two equal states mean nothing was added, removed,
    rewritten or touched in between."""
    state = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames:
            state[os.path.relpath(os.path.join(dirpath, name), root)] = ("dir",)
        for name in filenames:
            st = os.lstat(os.path.join(dirpath, name))
            state[os.path.relpath(os.path.join(dirpath, name), root)] = ("file", st.st_size, st.st_mtime_ns)
    return state


def describe(r):
    return "exit %s\nstdout: %s\nstderr: %s" % (r.returncode, r.stdout.strip(), r.stderr.strip())


VERDICTS = ("registered", "updated", "already-registered", "left-alone",
            "missing-files", "unsupported", "failed")


def verdict_line(r):
    """The line the mode exists to print, `<keyword>: <sentence>`, picked by its shape and
    not its position. (The engine's logger writes its INFO records to stdout too, each one
    starting with a `[` timestamp. Whether such a record may stand before or after the
    verdict is judged by one test of its own, so that it does not turn every test that
    merely reads the verdict red.) "" when stdout has no verdict at all."""
    for line in r.stdout.splitlines():
        if any(line.startswith(k + ":") for k in VERDICTS):
            return line
    return ""


@unittest.skipIf(_SELF_REGISTRATION_SKIP is not None, _SELF_REGISTRATION_SKIP or "")
class LinuxSelfRegistration(unittest.TestCase):
    """Runs the PACKAGED editor as `HorizonEditor --register-file-types`: the registration
    the editor runs at startup, synchronously, without a window and without touching the
    editor's config, against a throwaway HOME and XDG directories (the data home has a
    space in its name). It prints one line, `<keyword>: <sentence>`, and exits 0 for
    registered / updated / already-registered, 1 failed, 3 left-alone (the user chose
    another handler for .heproj), 4 missing-files, 5 unsupported.

    On Linux it must produce the files install_file_types.sh produces, where the script
    would put them (XDG_DATA_HOME if absolute, else $HOME/.local/share), and must not
    write a byte when the registration is current or the user's choice is someone else's.
    Nothing here launches the editor's GUI: the desktop is only asked what a .heproj is."""

    def setUp(self):
        self.tmp = os.path.realpath(tempfile.mkdtemp(prefix="he selfreg "))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.home = os.path.join(self.tmp, "home")
        self.data = os.path.join(self.tmp, "data home")
        self.config = os.path.join(self.tmp, "config")
        self.cwd = os.path.join(self.tmp, "cwd")
        # What /proc/self/exe says inside the editor, and so what the entry must point at.
        self.exe = os.path.realpath(PACKAGED_EDITOR)
        self.env = dict(os.environ)
        # No desktop session (xdg-mime would take its gio / KDE paths) and no display (the
        # mode has no window, so it must not need one): the generic freedesktop path only.
        for name in ("XDG_CURRENT_DESKTOP", "DESKTOP_SESSION", "KDE_FULL_SESSION",
                     "GNOME_DESKTOP_SESSION_ID", "DISPLAY", "WAYLAND_DISPLAY"):
            self.env.pop(name, None)
        self.env.update({
            "HOME": self.home,
            "XDG_DATA_HOME": self.data,
            "XDG_CONFIG_HOME": self.config,
            "XDG_DATA_DIRS": os.pathsep.join([self.data, "/usr/local/share", "/usr/share"]),
            # Whatever the mode does, it must not land in a real editor config.
            "HE_CONFIG_DIR": os.path.join(self.tmp, "editor config"),
        })
        os.makedirs(self.home)
        os.makedirs(self.cwd)

    # -- helpers ---------------------------------------------------------------------

    def installed(self, rel):
        return os.path.join(self.data, *rel.split("/"))

    def reset(self):
        """Back to a user who has registered nothing and chosen nothing."""
        for path in (self.data, self.config, os.path.join(self.home, ".local"),
                     os.path.join(self.home, ".config")):
            shutil.rmtree(path, True)

    def state(self):
        """Everything the registration could touch: the data home and the config home."""
        return tree_state(self.data), tree_state(self.config)

    def write_mimeapps(self, text, config_dir=None):
        config_dir = config_dir or self.config
        os.makedirs(config_dir, exist_ok=True)
        path = os.path.join(config_dir, "mimeapps.list")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        return path

    def age(self):
        """Back-date every file in the data and config homes to the year 2000, so that a
        rewrite shows up as a changed mtime even on a filesystem with coarse timestamps."""
        old = 946684800 * 10 ** 9
        for root in (self.data, self.config):
            for dirpath, _dirs, files in os.walk(root):
                for name in files:
                    os.utime(os.path.join(dirpath, name), ns=(old, old))

    def make_editor(self, dirname):
        """A stand-in for ANOTHER editor folder: the real FileTypes/ and a stub
        HorizonEditor that is never run (the installer only needs it to be executable)."""
        editor = os.path.join(self.tmp, dirname)
        shutil.copytree(LIN_DIR, os.path.join(editor, "FileTypes"),
                        ignore=shutil.ignore_patterns("check_*"))
        stub = os.path.join(editor, "HorizonEditor")
        with open(stub, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(stub, 0o755)
        return editor

    def run_script(self, script, env=None):
        return subprocess.run(["sh", script], env=env or self.env, cwd=self.cwd,
                              capture_output=True, text=True, timeout=60)

    def sweep_package(self, before):
        """Remove what a run left directly in the package folder (HorizonEngine.log and
        its rotated copies, dumps/) and say so: this check runs BEFORE the package is
        tarred up, and a dev log does not belong in a download. Not a failure, because a
        log next to the executable is how every editor run starts. A log that was already
        there is rotated, not created, so a developer's own logs are left alone."""
        had_log = any(n.startswith("HorizonEngine.log") for n in before)
        for name in sorted(set(os.listdir(PACKAGE_DIR)) - before):
            if had_log and name.startswith("HorizonEngine.log"):
                continue
            path = os.path.join(PACKAGE_DIR, name)
            if os.path.isdir(path) and not os.path.islink(path):
                shutil.rmtree(path, True)
            else:
                try:
                    os.remove(path)
                except OSError:
                    pass
            sys.stderr.write("note: --register-file-types left %s in the package folder; removed it\n" % name)

    def run_editor(self, env=None):
        """One `HorizonEditor --register-file-types`. A binary that cannot even start here
        (missing shared library, not executable) is an environment problem and skips; a
        crash, a hang or a wrong answer is the feature and fails."""
        before = set(os.listdir(PACKAGE_DIR))
        try:
            r = subprocess.run([PACKAGED_EDITOR, "--register-file-types"], env=env or self.env,
                               cwd=self.cwd, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", timeout=60)
        except OSError as e:
            self.skipTest("the packaged editor cannot be executed here: %s" % e)
        except subprocess.TimeoutExpired as e:
            self.fail("--register-file-types did not finish within 60 s\nstdout: %r\nstderr: %r"
                      % (e.stdout, e.stderr))
        finally:
            self.sweep_package(before)
        if r.returncode in (126, 127) or "error while loading shared libraries" in r.stderr:
            self.skipTest("the packaged editor cannot start here, an environment problem and not "
                          "the feature:\n" + describe(r))
        return r

    def assert_outcome(self, r, keyword, code):
        self.assertTrue(verdict_line(r).startswith(keyword + ":"),
                        "expected the verdict line to start with '%s:'\n%s" % (keyword, describe(r)))
        self.assertEqual(r.returncode, code, describe(r))

    def assert_current(self):
        """All four files are what the package's FileTypes/ makes of them for this editor:
        the entry is the template with its Exec and TryExec lines rewritten, the other
        three are byte-identical copies."""
        template = read_text(PACKAGE_DIR, "FileTypes", "horizon-editor.desktop")
        for rel, asset in REGISTERED_FILES:
            path = self.installed(rel)
            self.assertTrue(os.path.isfile(path), "missing after the registration: " + rel)
            if rel == DESKTOP_REL:
                self.assertEqual(read_bytes(path).decode("utf-8"), expected_desktop_text(template, self.exe), rel)
            else:
                self.assertEqual(read_bytes(path), read_bytes(PACKAGE_DIR, "FileTypes", asset), rel)

    # -- a user who has registered nothing -------------------------------------------

    def test_a_fresh_register_writes_the_four_files_and_says_registered(self):
        r = self.run_editor()
        self.assert_outcome(r, "registered", 0)
        keys = parse_desktop(read_text(self.installed(DESKTOP_REL)))
        self.assertEqual(keys["Exec"], '"%s" %%f' % self.exe.replace("%", "%%"))
        self.assertEqual(keys["TryExec"], self.exe)
        self.assertEqual(keys["MimeType"], MIME + ";")
        self.assertEqual(keys["Icon"], "horizon-editor")
        self.assert_current()
        # Not the editor's business in this mode: its config is neither read nor created.
        self.assertFalse(os.path.exists(os.path.join(self.env["HE_CONFIG_DIR"], "config.json")))

    def test_stdout_carries_the_verdict_line_and_nothing_else(self):
        """`registered: ...` is for scripts and CI to read, and the mode promises exactly
        that one line. The engine's logger writes INFO records to stdout by default (a
        missing optional helper such as gtk-update-icon-cache is logged at INFO), so this
        holds only if the mode silences the log console before it registers anything."""
        r = self.run_editor()
        self.assert_outcome(r, "registered", 0)
        lines = [l for l in r.stdout.splitlines() if l.strip()]
        self.assertEqual(len(lines), 1, "stdout must be the verdict line only\n" + describe(r))
        self.assertTrue(lines[0].startswith("registered:"), describe(r))

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-validate not installed")
    def test_the_registered_entry_passes_desktop_file_validate(self):
        self.assert_outcome(self.run_editor(), "registered", 0)
        r = subprocess.run(["desktop-file-validate", self.installed(DESKTOP_REL)],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(shutil.which("update-mime-database"), "update-mime-database not installed")
    def test_the_registered_mime_xml_builds_a_database_that_maps_the_extension(self):
        self.assert_outcome(self.run_editor(), "registered", 0)
        self.assertIn(MIME + ":*.heproj", read_text(self.data, "mime", "globs2"))
        self.assertIn(MIME + " application/json", read_text(self.data, "mime", "subclasses"))
        if shutil.which("update-desktop-database"):
            self.assertIn(MIME + "=horizon-editor.desktop;", read_text(self.data, "applications", "mimeinfo.cache"))

    def test_the_files_are_byte_identical_to_what_the_installer_script_writes(self):
        """The editor and install_file_types.sh are two implementations of one registration:
        run both from this package into separate homes and compare what they leave, the
        four files and the databases the helper tools build from them."""
        self.assert_outcome(self.run_editor(), "registered", 0)
        if self.exe != os.path.join(os.path.realpath(PACKAGE_DIR), "HorizonEditor"):
            self.skipTest("HorizonEditor is a symlink (%s): the script records the link, the editor its target"
                          % self.exe)
        other = os.path.join(self.tmp, "script data")
        other_config = os.path.join(self.tmp, "script config")
        env = dict(self.env)
        env.update({"HOME": os.path.join(self.tmp, "script home"), "XDG_DATA_HOME": other,
                    "XDG_CONFIG_HOME": other_config,
                    "XDG_DATA_DIRS": os.pathsep.join([other, "/usr/local/share", "/usr/share"])})
        os.makedirs(env["HOME"])
        r = self.run_script(os.path.join(PACKAGE_DIR, "FileTypes", "install_file_types.sh"), env)
        self.assertEqual(r.returncode, 0, describe(r))
        for rel, _asset in REGISTERED_FILES:
            self.assertEqual(read_bytes(self.installed(rel)), read_bytes(other, *rel.split("/")), rel)
        # Whatever update-mime-database, update-desktop-database and xdg-mime made of the
        # files for the script, they must have made of them for the editor: same files or none.
        pairs = [(os.path.join(self.data, "mime", "globs2"), os.path.join(other, "mime", "globs2")),
                 (os.path.join(self.data, "mime", "subclasses"), os.path.join(other, "mime", "subclasses")),
                 (os.path.join(self.data, "applications", "mimeinfo.cache"),
                  os.path.join(other, "applications", "mimeinfo.cache")),
                 (os.path.join(self.config, "mimeapps.list"), os.path.join(other_config, "mimeapps.list"))]
        for mine, theirs in pairs:
            name = os.path.basename(mine)
            self.assertEqual(os.path.isfile(mine), os.path.isfile(theirs),
                             "%s: the editor and the script disagree on whether it exists" % name)
            if os.path.isfile(mine):
                self.assertEqual(read_bytes(mine), read_bytes(theirs), name)

    @unittest.skipUnless(shutil.which("gio"), "the desktop's own resolver (glib's gio) is needed")
    def test_the_desktop_resolves_a_project_file_after_the_registration(self):
        self.assert_outcome(self.run_editor(), "registered", 0)
        project = os.path.join(self.tmp, "my project (1).heproj")
        with open(project, "w") as f:
            f.write("{}")
        # A hung glib call must not eat a ctest slot: every one of them has a timeout.
        info = subprocess.run(["gio", "info", "-a", "standard::content-type", project],
                              env=self.env, capture_output=True, text=True, timeout=30)
        self.assertIn("standard::content-type: " + MIME, info.stdout, info.stdout + info.stderr)
        handlers = subprocess.run(["gio", "mime", MIME], env=self.env, capture_output=True, text=True,
                                  timeout=30)
        self.assertIn("horizon-editor.desktop", handlers.stdout, handlers.stdout + handlers.stderr)
        # No `gio launch`: that would start the editor's GUI. The Exec line is checked above.

    # -- a user who has registered before ---------------------------------------------

    def test_a_second_run_says_already_registered_and_touches_nothing(self):
        self.assert_outcome(self.run_editor(), "registered", 0)
        self.age()
        before = self.state()
        r = self.run_editor()
        self.assert_outcome(r, "already-registered", 0)
        # Not one file written, not one mtime moved, not one stray file or directory.
        self.assertEqual(self.state(), before)
        self.assert_current()

    def test_a_registration_by_another_editor_folder_is_updated_to_this_editor(self):
        old = self.make_editor("Old Editor (1)")
        r = self.run_script(os.path.join(old, "FileTypes", "install_file_types.sh"))
        self.assertEqual(r.returncode, 0, describe(r))
        self.assertEqual(parse_desktop(read_text(self.installed(DESKTOP_REL)))["Exec"],
                         '"%s" %%f' % os.path.join(old, "HorizonEditor"))
        r = self.run_editor()
        self.assert_outcome(r, "updated", 0)
        self.assertEqual(parse_desktop(read_text(self.installed(DESKTOP_REL)))["Exec"],
                         '"%s" %%f' % self.exe.replace("%", "%%"))
        self.assert_current()
        # The registration is current now: the next start has nothing to do.
        self.assert_outcome(self.run_editor(), "already-registered", 0)

    def test_a_missing_or_stale_file_is_repaired(self):
        self.assert_outcome(self.run_editor(), "registered", 0)

        def write(data):
            def tamper(path):
                with open(path, "wb") as f:
                    f.write(data)
            return tamper

        def point_elsewhere(path):
            with open(path, "wb") as f:
                f.write(read_text(path).replace(self.exe, "/somewhere/else/HorizonEditor").encode("utf-8"))

        # (what happened, which file, how it was spoiled, what the editor must say)
        cases = [("the MIME xml is deleted", MIME_XML_REL, os.remove, "updated"),
                 ("the file icon is not the package's", MIME_ICON_REL, write(b"not a png"), "updated"),
                 ("the app icon is deleted", APP_ICON_REL, os.remove, "updated"),
                 ("the entry points at another folder", DESKTOP_REL, point_elsewhere, "updated"),
                 ("the entry is gone, the rest is there", DESKTOP_REL, os.remove, "registered")]
        for what, rel, spoil, keyword in cases:
            with self.subTest(what):
                spoil(self.installed(rel))
                r = self.run_editor()
                self.assert_outcome(r, keyword, 0)
                self.assert_current()
        self.assert_outcome(self.run_editor(), "already-registered", 0)

    # -- a user who chose another application -----------------------------------------

    def test_a_foreign_default_handler_is_left_alone_and_nothing_is_written(self):
        for text in (FOREIGN_DEFAULT,
                     # the FIRST desktop id is the default: ours behind another one is not ours
                     "[Default Applications]\napplication/x-heproj=other.desktop;horizon-editor.desktop;\n"):
            with self.subTest(text.splitlines()[1]):
                path = self.write_mimeapps(text)
                before = self.state()
                r = self.run_editor()
                self.assert_outcome(r, "left-alone", 3)
                self.assertFalse(os.path.exists(self.installed(DESKTOP_REL)))
                self.assertEqual(read_bytes(path), text.encode("utf-8"))
                self.assertEqual(self.state(), before, "left-alone must not write anything")

    def test_other_mimeapps_entries_are_not_a_foreign_handler(self):
        """The positive control for the test above: a mimeapps.list that exists is not
        enough to be left alone, only another default for THIS type in the default group."""
        for text in ("[Added Associations]\napplication/x-heproj=other.desktop;\n"
                     "[Default Applications]\ntext/plain=other.desktop;\n",
                     "[Default Applications]\napplication/x-heproj=horizon-editor.desktop;other.desktop;\n"):
            with self.subTest(text.replace("\n", " | ")):
                self.reset()
                self.write_mimeapps(text)
                r = self.run_editor()
                self.assert_outcome(r, "registered", 0)
                self.assert_current()

    # -- where the files go, when the environment does not say -----------------------

    def test_the_data_home_falls_back_to_dot_local_share_when_unset_or_relative(self):
        """The script ignores a relative XDG_DATA_HOME, as the Desktop Entry spec does, and
        so must the editor: most users have none set at all."""
        for label, value in (("unset", None), ("relative", os.path.join("relative", "data"))):
            with self.subTest(label):
                self.reset()
                env = dict(self.env)
                if value is None:
                    del env["XDG_DATA_HOME"]
                else:
                    env["XDG_DATA_HOME"] = value
                self.assert_outcome(self.run_editor(env), "registered", 0)
                fallback = os.path.join(self.home, ".local", "share")
                for rel, _asset in REGISTERED_FILES:
                    self.assertTrue(os.path.isfile(os.path.join(fallback, *rel.split("/"))), rel)
                self.assertFalse(os.path.exists(self.data))
                # Not "relative to wherever the editor was started" either.
                self.assertFalse(os.path.exists(os.path.join(self.cwd, "relative", "data", *DESKTOP_REL.split("/"))))

    def test_the_config_home_falls_back_to_dot_config_when_unset(self):
        env = dict(self.env)
        del env["XDG_CONFIG_HOME"]
        self.write_mimeapps(FOREIGN_DEFAULT, os.path.join(self.home, ".config"))
        r = self.run_editor(env)
        self.assert_outcome(r, "left-alone", 3)
        self.assertFalse(os.path.exists(self.installed(DESKTOP_REL)))


if __name__ == "__main__":
    unittest.main()
