#!/usr/bin/env bash
# Build Gamescope for Hermes the same way for every package.
#
#   build.sh compile <build-dir> [meson options...]
#   build.sh install <build-dir> <destdir>
#
# It installs under /usr/lib/hermes-gamescope, beside the distribution's
# gamescope, with wlroots, libliftoff, vkroots and libdisplay-info built from
# the pinned submodules so no package follows the system's wlroots.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
prefix=/usr/lib/hermes-gamescope

action=${1:?usage: build.sh compile|install <build-dir> ...}
build=${2:?usage: build.sh compile|install <build-dir> ...}
shift 2

case "$action" in
  compile)
    rm -rf "$build"
    meson setup "$build" "$root" \
      --prefix="$prefix" \
      --libdir=lib \
      --buildtype=release \
      -Db_pie=true \
      -Dforce_fallback_for=libliftoff,vkroots,wlroots,libdisplay-info \
      -Dpipewire=enabled \
      -Denable_openvr_support=false \
      -Denable_tests=false \
      -Dbenchmark=disabled \
      "$@"
    meson compile -C "$build"
    ;;
  install)
    dest=${1:?usage: build.sh install <build-dir> <destdir>}
    meson install -C "$build" --destdir "$dest" --skip-subprojects
    install -Dm644 "$root/LICENSE" "$dest/usr/share/licenses/gamescope-hermes/LICENSE"
    install -Dm644 "$root/HERMES.md" "$dest/usr/share/doc/gamescope-hermes/HERMES.md"
    ;;
  *)
    echo "usage: build.sh compile|install <build-dir> ..." >&2
    exit 2
    ;;
esac
