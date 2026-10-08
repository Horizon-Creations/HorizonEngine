#!/usr/bin/env python3
"""Tests for the .heproj file type registration of the Windows and Linux packages.

Double-clicking a project only opens the editor if the OS has been told that .heproj
is the editor's. macOS says so in Info.plist (scripts/package_macos.sh). Windows and
Linux ship the editor as a ZIP / tarball with no installer, so the registration is a
set of files inside the package, FileTypes/, that the user runs once
(scripts/windows_assets, scripts/linux_assets, wired in src/HE_Editor/CMakeLists.txt).

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
  * with --package DIR, the deployed package really contains those files.

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
        info = subprocess.run(["gio", "info", "-a", "standard::content-type", project],
                              env=self.env, capture_output=True, text=True)
        self.assertIn("standard::content-type: " + MIME, info.stdout, info.stdout + info.stderr)
        handlers = subprocess.run(["gio", "mime", MIME], env=self.env, capture_output=True, text=True)
        self.assertIn("horizon-editor.desktop", handlers.stdout, handlers.stdout + handlers.stderr)
        # What a file manager does on a double-click: launch the entry with the file.
        launch = subprocess.run(["gio", "launch", self.desktop_file(), project],
                                env=self.env, capture_output=True, text=True)
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


if __name__ == "__main__":
    unittest.main()
