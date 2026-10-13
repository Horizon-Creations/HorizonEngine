#!/usr/bin/env bash
# ─── git auto-install, Linux end to end, in a WSL distro that has never seen git ──
# Two halves, because the machine under test must not have the toolchain:
#
#   bundle  (in a build distro WITH git, after building HorizonSourceControl)
#       compiles git_install_e2e.cpp against the build tree and copies it, every
#       non-libc library it needs and a small committed repository into <out>.
#   run     (in the fresh distro, as the user the case needs)
#       unpacks the repository as that user (git refuses a repo owned by another
#       uid, which would look like "no repo") and runs one case.
#
# The fresh distro (Windows host; ubuntu-base has no git, no sudo, no pkexec):
#   curl -LO https://cdimage.ubuntu.com/ubuntu-base/releases/26.04/release/ubuntu-base-26.04.1-base-amd64.tar.gz
#   wsl --import hw195-nogit C:\hw195\fresh\distro ubuntu-base-26.04.1-base-amd64.tar.gz --version 2
#   (as root) printf '[interop]\nappendWindowsPath=false\n' > /etc/wsl.conf; useradd -m -u 1000 tester
#   wsl --terminate hw195-nogit
# appendWindowsPath=false matters: otherwise the Windows PATH (Git for Windows
# included) is visible inside the distro.
#
# Cases, in this order (the last one installs for real):
#   run <out> norights   as tester, no pkexec    -> Apt/Unavailable, manual sudo line, nothing started
#   run <out> pkexec     as tester, pkexec added -> Apt/Pkexec, launch through pkexec /bin/sh -c
#   run <out> root       as root                 -> Apt/AlreadyRoot, apt-get installs git, repo shows up
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

case "${1:-}" in
bundle)
	src=$2; build=$3; out=$4
	mkdir -p "$out/lib"
	g++ -std=c++20 -O1 -o "$out/git_install_e2e" "$here/git_install_e2e.cpp" \
		-I"$src/src/HE_Core/include" -I"$src/src/HE_SourceControl/include" \
		-L"$build/src/HE_SourceControl" -L"$build/src/HE_Core" \
		-lHorizonSourceControl -lHorizonCore -Wl,-rpath,'$ORIGIN/lib'
	# Everything the loader resolves, minus the libc family every Ubuntu has. The
	# rpath points at the still empty lib/, so ldd needs the build tree's dirs.
	libdirs="$(find "$build" -name '*.so*' -not -path '*/CMakeFiles/*' -printf '%h\n' | sort -u | paste -sd:)"
	LD_LIBRARY_PATH="$libdirs" ldd "$out/git_install_e2e" | tee "$out/ldd.txt" |
		awk '/=> \//{print $3}' |
		{ grep -vE '/(libc|libm|libstdc\+\+|libgcc_s|ld-linux-x86-64)\.so' || true; } |
		while read -r so; do cp -L "$so" "$out/lib/"; done
	if grep -q "not found" "$out/ldd.txt"; then echo "unresolved:"; grep "not found" "$out/ldd.txt"; exit 1; fi
	rm -rf "$out/repo-src" && mkdir -p "$out/repo-src/sample-project"
	( cd "$out/repo-src/sample-project" && git init -q -b main &&
	  echo "a project in a repository" > README.md && mkdir -p Content && echo x > Content/a.txt &&
	  git add -A && git -c user.name=e2e -c user.email=e2e@example.invalid commit -qm "initial" )
	tar -C "$out/repo-src" -czf "$out/sample-project.tgz" sample-project
	echo "bundle in $out: $(ls "$out/lib" | wc -l) libraries"
	;;
run)
	out=$2; what=$3
	work="$HOME/e2e-$what"; rm -rf "$work"; mkdir -p "$work"
	# --no-same-owner: root would otherwise keep the archive's uid, and git then
	# answers "dubious ownership", which GitService reads as "no repository".
	tar -C "$work" --no-same-owner -xzf "$out/sample-project.tgz"
	# RUNPATH only covers the executable's direct needs, not libcurl's own.
	export LD_LIBRARY_PATH="$out/lib"
	echo "== case $what as $(id -un); git on PATH: $(command -v git || echo none)"
	case "$what" in
	norights) "$out/git_install_e2e" "$work/sample-project/Content" --install \
	              --expect-route apt --expect-elevation Unavailable ;;
	pkexec)   timeout 300 "$out/git_install_e2e" "$work/sample-project/Content" --install \
	              --expect-route apt --expect-elevation Pkexec ;;
	root)     "$out/git_install_e2e" "$work/sample-project/Content" --install \
	              --expect-route apt --expect-elevation AlreadyRoot
	          echo "== after: $(command -v git) / $(git --version)" ;;
	*) echo "unknown case $what"; exit 2 ;;
	esac
	;;
*)
	echo "usage: $0 bundle <src> <build> <out> | run <out> <norights|pkexec|root>"; exit 2 ;;
esac
