#!/usr/bin/env bash
# Build a newer Wayland stack into Gamescope's private prefix, for releases
# whose own is older than wlroots needs (Ubuntu 24.04 ships libwayland 1.22
# and libdrm 2.4.125; wlroots 0.20 requires 1.24 and 2.4.129). The libraries are installed where the package
# puts them, and Gamescope is linked with a DT_RPATH to that directory, which
# the loader also honours for the libraries Gamescope loads (SDL2, libdecor),
# so the whole process uses one libwayland. libwayland is backwards
# compatible, so libraries built against the release's 1.22 work with it.
#
#   bundle-wayland.sh <prefix>
#
# Prints the meson options Gamescope needs on stdout.
set -euo pipefail
prefix=${1:?prefix}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Each piece builds against the ones before it: wayland-protocols needs the
# new wayland-scanner, and wlroots later needs all three.
export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig:${prefix}/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PATH="${prefix}/bin:${PATH}"

latest_tag() {
  git ls-remote --tags --refs "$1" | sed 's|.*refs/tags/||' | grep -E "$2" | sort -V | tail -n1
}

build() {
  local name=$1 url=$2 tag=$3
  shift 3
  echo "Building ${name} ${tag}" >&2
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$url" "$work/$name" >&2
  meson setup "$work/$name/build" "$work/$name" --prefix="$prefix" --libdir=lib --buildtype=release "$@" >&2
  ninja -C "$work/$name/build" install >&2
}

build wayland https://gitlab.freedesktop.org/wayland/wayland.git \
  "$(latest_tag https://gitlab.freedesktop.org/wayland/wayland.git '^1\.2[4-9]\.[0-9]+$')" \
  -Ddocumentation=false -Dtests=false -Ddtd_validation=false
build wayland-protocols https://gitlab.freedesktop.org/wayland/wayland-protocols.git \
  "$(latest_tag https://gitlab.freedesktop.org/wayland/wayland-protocols.git '^1\.[0-9]+$')" \
  -Dtests=false
# Only the core library: the vendor ones (libdrm_amdgpu and so on) stay the
# release's, and load against this libdrm, which is backwards compatible.
build libdrm https://gitlab.freedesktop.org/mesa/drm.git \
  "$(latest_tag https://gitlab.freedesktop.org/mesa/drm.git '^libdrm-2\.4\.[0-9]+$')" \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
  -Dman-pages=disabled -Dvalgrind=disabled -Dcairo-tests=disabled -Dtests=false
build pixman https://gitlab.freedesktop.org/pixman/pixman.git \
  "$(latest_tag https://gitlab.freedesktop.org/pixman/pixman.git '^pixman-0\.(4[3-9]|[5-9][0-9])\.[0-9]+$')" \
  -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled

printf '%s\n' \
  "-Dc_link_args=-Wl,--disable-new-dtags,-rpath,${prefix}/lib" \
  "-Dcpp_link_args=-Wl,--disable-new-dtags,-rpath,${prefix}/lib"
