#!/usr/bin/env bash
# Turn a staged tree into gamescope-hermes-<version>-1.fc<N>.x86_64.rpm, the
# way Hermes' own .rpm is made: %files is generated from the tree, and
# rpmbuild derives Requires from what the binaries link, so each Fedora
# release gets the package built against its own libraries.
#
#   package-rpm.sh <staged-tree> <version> <output-dir>
set -euo pipefail
tree=$(realpath "${1:?staged tree}")
version=${2:?version}
out=$(realpath "${3:?output dir}")
pkg=gamescope-hermes

top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT
mkdir -p "$top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

files=$(cd "$tree" && {
  find . -type d -path './usr/lib/hermes-gamescope*' -printf '%%dir /%P\n'
  find . \( -type f -o -type l \) -printf '/%P\n'
} | sort)

cat >"$top/SPECS/${pkg}.spec" <<SPEC
Name:           ${pkg}
Version:        ${version}
Release:        1%{?dist}
Summary:        Gamescope build for Hermes isolated sessions
License:        BSD-2-Clause
URL:            https://github.com/MrOz59/gamescope
Requires:       xorg-x11-server-Xwayland
Requires:       vulkan-loader
Requires:       hwdata
Requires:       xkeyboard-config

# Built by the job that calls this script and only copied in here.
%global debug_package %{nil}
# The WSI layer in the private prefix is not a library anything else may
# link, so it is not offered to the rest of the system.
%global __provides_exclude_from ^/usr/lib/hermes-gamescope/.*\$

%description
Gamescope with the patches Hermes' isolated sessions need: scanout on a
different DRM device than it renders with (--prefer-drm) and input from the
session's own seat (XDG_SEAT). Installed under /usr/lib/hermes-gamescope,
beside the distribution's gamescope.

%install
mkdir -p %{buildroot}
cp -a ${tree}/. %{buildroot}/

%files
%defattr(-,root,root,-)
${files}
SPEC

rpmbuild --define "_topdir ${top}" -bb "$top/SPECS/${pkg}.spec"
cp "$top"/RPMS/*/*.rpm "$out/"
