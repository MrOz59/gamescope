#!/usr/bin/env bash
# Turn a staged tree into gamescope-hermes_<version>_ubuntu<release>_amd64.deb,
# the way Hermes' own .deb is made: Depends come from what the binaries link
# (dpkg-shlibdeps), and the release goes in the file name so each Ubuntu
# release gets the package built against its own libraries.
#
#   package-deb.sh <staged-tree> <version> <ubuntu-release> <output-dir>
set -euo pipefail
tree=$(realpath "${1:?staged tree}")
version=${2:?version}
release=${3:?ubuntu release}
out=$(realpath "${4:?output dir}")
pkg=gamescope-hermes

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -a "$tree" "$work/root"
install -Dm644 "$work/root/usr/share/licenses/${pkg}/LICENSE" "$work/root/usr/share/doc/${pkg}/copyright"
mkdir -p "$work/root/DEBIAN"

elves=()
while IFS= read -r -d '' file; do
  if [ "$(head -c4 "$file" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ]; then
    elves+=("$file")
  fi
done < <(find "$work/root/usr/lib/hermes-gamescope" -type f -print0)
[ "${#elves[@]}" -gt 0 ] || { echo "no binaries in the staged tree" >&2; exit 1; }

mkdir -p "$work/shlibs/debian"
printf 'Source: %s\nPackage: %s\nArchitecture: amd64\n' "$pkg" "$pkg" >"$work/shlibs/debian/control"
depends=$(cd "$work/shlibs" &&
  dpkg-shlibdeps -O --ignore-missing-info -l"$work/root/usr/lib/hermes-gamescope/lib" "${elves[@]}" |
  sed 's/^shlibs:Depends=//')
[ -n "$depends" ] || { echo "dpkg-shlibdeps produced no dependencies" >&2; exit 1; }
echo "Depends: ${depends}"

cat >"$work/root/DEBIAN/control" <<CONTROL
Package: ${pkg}
Version: ${version}
Section: x11
Priority: optional
Architecture: amd64
Depends: ${depends}, libvulkan1, hwdata, xkb-data, xwayland
Maintainer: Hermes
Homepage: https://github.com/MrOz59/gamescope
Description: Gamescope build for Hermes isolated sessions
 Gamescope with the patches Hermes' isolated sessions need: scanout on a
 different DRM device than it renders with (--prefer-drm) and input from the
 session's own seat (XDG_SEAT). Installed under /usr/lib/hermes-gamescope,
 beside the distribution's gamescope.
CONTROL

dpkg-deb --root-owner-group --build "$work/root" "${out}/${pkg}_${version}_ubuntu${release}_amd64.deb"
