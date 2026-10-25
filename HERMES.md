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
and Hermes looks there first. Both `/usr/bin/gamescope-hermes` and
`/usr/lib/hermes-gamescope/bin/gamescope` use a launcher that prepends the private
helper directory to `PATH` and its share directory to `XDG_DATA_DIRS`. The real
binary is `/usr/lib/hermes-gamescope/libexec/gamescope`; invoke the launcher so
its helpers and WSI layer are available. Hermes' installer (`scripts/install.sh` in the
Hermes repository) offers it. CI builds a package per distribution release,
the same way Hermes' own packages are built, and keeps them on the rolling
[`hermes-nightly`](https://github.com/MrOz59/gamescope/releases/tag/hermes-nightly)
release:

| Distribution | Asset | Installs |
|:-------------|:------|:---------|
| Arch / CachyOS | `gamescope-hermes-<version>-1-x86_64.pkg.tar.zst` | `sudo pacman -U ./gamescope-hermes-*.pkg.tar.zst` |
| Ubuntu 26.04 | `gamescope-hermes_<version>_ubuntu<release>_amd64.deb` | `sudo apt install ./gamescope-hermes_*.deb` |
| Fedora 43, 44 | `gamescope-hermes-<version>-1.fc<release>.x86_64.rpm` | `sudo dnf install ./gamescope-hermes-*.rpm` |

Choose only the package matching your distribution release and architecture.
Ubuntu 24.04 is pending a separate backport: the pinned wlroots 0.20 requires
Wayland >= 1.24, libdrm >= 2.4.129, libxkbcommon >= 1.8 and pixman >= 0.43,
plus wayland-protocols >= 1.47. Its stock libraries do not meet that baseline.
Do not install a 26.04 package on 24.04 or mix a Fedora 43/44 package with another
release. Arch/CachyOS packages follow the current rolling repositories.

Debian and RPM package dependencies include the Vulkan loader explicitly:
Gamescope loads `libvulkan.so.1` with `dlopen`, which is invisible to automatic
ELF dependency scanners. They also include hardware IDs, keyboard data and
Xwayland. Install the Vulkan driver/ICD appropriate for your GPU separately.

To check the installed entry point without starting a compositor:

```sh
gamescope-hermes --help
gamescope-hermes --version
```

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

## Session integration and validation

`--prefer-drm` takes a **primary** KMS node (`/dev/dri/cardN` or a by-path link),
not a render node. `--prefer-vk-device` continues to select the rendering GPU.
Independent compositors need separate DRM cards, a configured private broker,
and input devices tagged for the intended `XDG_SEAT`. This fork does not create
those cards, brokers, or input devices; Hermes and Hermes-KMS provide that setup.
It is session plumbing, not a security boundary for mutually untrusted users.

The seatd broker for an isolated card must run with `SEATD_VTBOUND=0` so it does
not take over the host's VT. Set `SEATD_SOCK` and `LIBSEAT_BACKEND=seatd` to select
the private broker. Use the current Hermes launcher/integration to pass its card,
seat and client mode rather than copying a physical display's device number.

Package CI builds in each target release, then installs each package in a fresh
runtime container and checks ELF linkage, the Vulkan loader, data files,
`--help`/`--version`, and the fork's `--prefer-drm` option. A launcher regression
also checks private helper lookup, custom/default data paths, argument forwarding
and exit status. Only packages passing the runtime checks are published; a
failed distribution keeps its previous asset. Check the commit hash in each
filename, since a rolling release can contain packages from different runs.

These checks need no GPU. Real-session validation must still cover KMS scanout,
input isolation, client touch, Vulkan driver behavior, sustained streaming and
any HDR path claimed by the consuming Hermes configuration.

## Keeping up with upstream

Rebase `hermes` on `master`, drop a patch once upstream carries it, and bump
`packaging/hermes/VERSION` when the base passes a new Gamescope release.
