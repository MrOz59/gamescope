# Gamescope for Hermes

This is the `hermes` branch of MrOz59/gamescope: upstream
[ValveSoftware/gamescope](https://github.com/ValveSoftware/gamescope) `master`
plus what [Hermes](https://github.com/MrOz59/Hermes) isolated sessions need to
run a game in its own Gamescope, on its own Hermes-KMS card and seat, while the
host's desktop stays in use.

| Patch | Why |
| --- | --- |
| `--prefer-drm` / `GAMESCOPE_PREFER_DRM` ([#2220](https://github.com/ValveSoftware/gamescope/pull/2220), by relil) | Scan out on the session's Hermes-KMS card while rendering on the real GPU. Upstream opens the GPU it renders with, which is the host's display. |
| libinput seat from `XDG_SEAT` (from d3adb5, [Hermes#57](https://github.com/MrOz59/Hermes/issues/57)) | seatd names every seat `seat0`, so without it the session reads the host's keyboard and mouse. |
| `wl_touch.frame` after passthrough touch (from d3adb5) | KWin and Qt apply touch only on the frame event, so taps never ended. |

## Install

It installs under `/usr/lib/hermes-gamescope`, beside the system's Gamescope,
and Hermes looks there first. Hermes' installer (`scripts/install.sh` in the
Hermes repository) offers it. By hand on Arch or CachyOS:

```sh
git clone --recursive --branch hermes https://github.com/MrOz59/gamescope.git
cd gamescope/packaging/hermes
makepkg -si
```

Elsewhere, build it into `/opt/hermes-gamescope`, the other prefix Hermes
checks (the build dependencies are those in `packaging/hermes/PKGBUILD`):

```sh
meson setup build --prefix=/opt/hermes-gamescope --buildtype=release \
  -Dforce_fallback_for=libliftoff,vkroots,wlroots,libdisplay-info \
  -Denable_openvr_support=false -Denable_tests=false
ninja -C build && sudo meson install -C build --skip-subprojects
```

## Keeping up with upstream

Rebase `hermes` on `master` and drop a patch once upstream carries it.
