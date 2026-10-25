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
Hermes repository) offers it. CI builds a package per distribution release,
the same way Hermes' own packages are built, and keeps them on the rolling
[`hermes-nightly`](https://github.com/MrOz59/gamescope/releases/tag/hermes-nightly)
release:

| Distribution | Asset | Installs |
|:-------------|:------|:---------|
| Arch / CachyOS | `gamescope-hermes-<version>-1-x86_64.pkg.tar.zst` | `sudo pacman -U ./gamescope-hermes-*.pkg.tar.zst` |
| Ubuntu 24.04, 26.04 | `gamescope-hermes_<version>_ubuntu<release>_amd64.deb` | `sudo apt install ./gamescope-hermes_*.deb` |
| Fedora 43, 44 | `gamescope-hermes-<version>-1.fc<release>.x86_64.rpm` | `sudo dnf install ./gamescope-hermes-*.rpm` |

To build the Arch package yourself:

```sh
git clone --recursive --branch hermes https://github.com/MrOz59/gamescope.git
cd gamescope/packaging/hermes
makepkg -si
```

Every package is built by `packaging/hermes/build.sh`; `package-deb.sh` and
`package-rpm.sh` turn its staged tree into a `.deb` or `.rpm`. The version is
the upstream release the branch is based on (`packaging/hermes/VERSION`) plus
the commit count and hash.

## Keeping up with upstream

Rebase `hermes` on `master`, drop a patch once upstream carries it, and bump
`packaging/hermes/VERSION` when the base passes a new Gamescope release.
