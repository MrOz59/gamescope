#!/usr/bin/env bash
# Run after installing the package in a fresh target-distribution container.
# This checks userspace startup; it does not certify DRM/input/GPU streaming.
set -euo pipefail
trap 'status=$?; printf "Package check failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2; exit "$status"' ERR
archive=${1:?usage: check-installed.sh PACKAGE}
prefix=/usr/lib/hermes-gamescope
test -x "$prefix/bin/gamescope"
test -x "$prefix/libexec/gamescope"
test -L /usr/bin/gamescope-hermes
test "$(readlink -f /usr/bin/gamescope-hermes)" = "$prefix/bin/gamescope"
for helper in gamescopereaper gamescope-type gamescopectl gamescopestream gamescope-hermes-client; do
  test -x "$prefix/bin/$helper"
done
test -s "$prefix/share/gamescope/scripts/00-gamescope/common/util.lua"
test -s "$prefix/share/gamescope/looks/grayscale_g22.cube"
# Distributions do not all install these databases under the same prefix.
test -s /usr/share/hwdata/pnp.ids || test -s /usr/share/misc/pnp.ids
test -s /usr/share/X11/xkb/rules/evdev || test -s /usr/share/xkeyboard-config-2/rules/evdev

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Minimal images can exclude documentation (dpkg path-exclude, RPM nodocs or
# pacman's NoExtract). Check that the archive ships it, independent of that
# installation policy, rather than requiring excluded files to exist on disk.
case "$archive" in
  *.deb) dpkg-deb --fsys-tarfile "$archive" | tar -tf - | sed 's|^\./|/|' > "$work/files" ;;
  *.rpm) rpm -qpl "$archive" > "$work/files" ;;
  *.pkg.tar.zst) pacman -Qqlp "$archive" > "$work/files" ;;
  *) printf 'Unsupported package archive: %s\n' "$archive" >&2; exit 1 ;;
esac
grep -Fxq /usr/share/doc/gamescope-hermes/HERMES.md "$work/files"
grep -Fxq /usr/share/licenses/gamescope-hermes/LICENSE "$work/files"

for file in "$prefix/libexec/gamescope" "$prefix"/bin/* "$prefix"/lib/*.so; do
  # Skip shell launchers. These are our own build products, not untrusted ELF.
  [ "$(head -c4 "$file" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
  if ! ldd "$file" > "$work/ldd" 2>&1; then
    printf 'Failed to inspect ELF linkage: %s\n' "$file" >&2
    cat "$work/ldd" >&2
    exit 1
  fi
  if grep -q 'not found' "$work/ldd"; then
    cat "$work/ldd" >&2
    exit 1
  fi
done
# Vulkan is loaded through dlopen, so ELF-generated package dependencies miss it.
ldconfig -p > "$work/loader-cache"
grep -q 'libvulkan.so.1' "$work/loader-cache"
manifest="$prefix/share/vulkan/implicit_layer.d/VkLayer_FROG_gamescope_wsi.x86_64.json"
test -s "$manifest"
grep -Fq "$prefix/lib/libVkLayer_FROG_gamescope_wsi_x86_64.so" "$manifest"
if ! gamescope-hermes --help > "$work/help" 2>&1; then
  cat "$work/help" >&2
  exit 1
fi
grep -q -- '--prefer-drm' "$work/help"
grep -q -- '--hermes-socket' "$work/help"
"$prefix/bin/gamescope" --version
printf '%s\n' 'Installed package: PASS (no compositor/GPU session started)'
