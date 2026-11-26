<p align="center">
  <img src="src/assets/icons/Flux.svg" alt="Flux icon" width="256" height="256">
  <br>
  <strong>Flux</strong>
</p>

<p align="center">
  A lightweight Linux software center with a native desktop interface.
</p>

Flux is a lightweight Linux software center with a native desktop interface. It provides package search and common package operations through a clean, responsive UI.

> **Current support:** Package search, installation, removal, and cache clearing are supported on Arch Linux (`pacman`) and Fedora-family distributions (DNF). DNF search uses repository package names. DNF4 requires `dnf-plugins-core` for `repoquery`; DNF5 includes it.

## Features

- Search packages and view descriptions and installation status.
- Install and remove packages through Polkit authentication.
- Clear the pacman or DNF package cache.
- Customize themes, fonts, font size, descriptions, animations, and scrolling.
- Navigate the interface using the keyboard.
- Build native Arch (`.pkg.tar.zst`) and Fedora (`.rpm`) packages.

## Requirements

- Linux with OpenGL 4.6 support.
- CMake 3.28+ and a C++26 compiler (Clang recommended; GCC supported).
- OpenGL development files, GLFW 3.3+, FreeType, Fontconfig, libpng, librsvg, libepoxy, and pkg-config.
- For package operations: `pacman` or `dnf`, plus Polkit (`pkexec`).
- DNF4 additionally requires `dnf-plugins-core`.
- Optional: [`just`](https://just.systems/) and [Nix](https://nixos.org/) with flakes enabled.

### Build dependencies

Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake clang glfw-x11 freetype2 fontconfig libpng librsvg libepoxy pkgconf
```

Fedora:

```sh
sudo dnf install clang cmake make glfw-devel freetype-devel fontconfig-devel libpng-devel librsvg2-devel libepoxy-devel libglvnd-devel pkgconf-pkg-config
```

The GLFW package name may differ depending on your window-system backend.

## Build and run

With `just`:

```sh
just build
just run
```

Or use CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/flux
```

Show version and available options:

```sh
./build/bin/flux --help
```

Use `--force` to launch the interface on unsupported distributions. Package management will remain unavailable:

```sh
./build/bin/flux --force
```

The default build uses Release mode. Set `CXX` to select a compiler (`CXX=g++ just build`) or use `BUILD_TYPE=Debug` for a debug build.

## Install with Nix

The repository includes a Nix flake with all required build dependencies:

```sh
nix build .
nix run github:dimunyx/Flux
nix profile install github:dimunyx/Flux
```

For a local checkout, use `nix build .`, `nix run .`, or `nix profile install .`. Supported systems: `x86_64-linux` and `aarch64-linux`.

The package uses Clang and ThinLTO without `-march=native`, allowing reproducible builds and remote builders. Commit the generated `flake.lock` to pin the `nixpkgs` revision.

For development:

```sh
nix develop
just build
```

> Flux uses system commands under `/usr/bin` for package operations. These work on supported Arch and Fedora-family systems. On NixOS, launch with `--force`; package operations are unavailable.

## Fedora installation

To add the Flux DNF repository:

```sh
sudo tee /etc/yum.repos.d/flux.repo >/dev/null <<'EOF'
[flux]
name=Flux packages
baseurl=https://dimunyx.github.io/Flux/rpm/
enabled=1
gpgcheck=0
repo_gpgcheck=0
EOF
```

Install and update Flux:

```sh
sudo dnf makecache --refresh
sudo dnf install flux
sudo dnf upgrade flux
```

The repository is published automatically through GitHub Actions. It also supports other DNF/RPM-compatible distributions. Packages are currently unsigned, so signature checks are disabled in the example configuration.

To build an RPM locally, install `rpm-build` and run:

```sh
just rpm
```

The package is written to `packaging/rpm/`.

## Arch Linux package and repository

Build a package from the current checkout:

```sh
just pkg
```

Or build and install it directly:

```sh
cd packaging/arch
makepkg -si
```

To create a local pacman repository:

```sh
just repo
```

Or manually:

```sh
mkdir -p repo
cp packaging/arch/*.pkg.tar.zst repo/
repo-add repo/flux.db.tar.gz repo/flux-*.pkg.tar.zst
```

Configure pacman to use the local repository:

```ini
[flux]
SigLevel = Optional TrustAll
Server = file:///home/you/dev/Flux/repo
```

Alternatively, serve it over HTTP:

```sh
just serve
```

```ini
[flux]
SigLevel = Optional TrustAll
Server = http://localhost:8080
```

Then install Flux:

```sh
sudo pacman -Syu flux
```

The Arch package uses `-DFLUX_NATIVE=OFF` to avoid CPU-specific builds. Packages are unsigned; use GPG signatures for public repositories.

The `.github/workflows/pacman-repo.yml` workflow builds Arch and RPM repositories on pushes to `main` and deploys them to GitHub Pages. Enable **Settings → Pages → Source: GitHub Actions** to publish them.

The published Arch repository can be added with:

```ini
[flux]
SigLevel = Optional TrustAll
Server = https://<user>.github.io/Flux/
```

## Localization and settings

Flux includes English, Russian, and Romanian translations in `src/assets/locales/{en,ru,ro}.json`.

Choose a language in **Settings → Appearance**, or let Flux detect `LC_ALL`, `LC_MESSAGES`, or `LANG`. Override it for one launch with:

```sh
FLUX_LANG=ru flux
```

Settings are stored in `~/.config/flux/config.json` (or `$XDG_CONFIG_HOME/flux/config.json`). Flux automatically migrates the previous line-based settings file when the JSON config is absent.

On first launch, Flux creates `~/.config/flux/themes/default.json`. Add custom JSON themes there and select them in **Settings → Themes**. Changes apply immediately.

Example theme:

```json
{
  "name": "Ocean",
  "light": false,
  "background": "#111827",
  "header": "#1f2937",
  "surface": "#202c3d",
  "elevated": "#2d3a4d",
  "outline": "#64748b",
  "primary": "#60a5fa",
  "primary_container": "#273b55",
  "text": "#e5edf8",
  "secondary_text": "#a8b5c8",
  "selection": "#334155",
  "on_primary": "#10243a"
}
```

`on_primary` is optional and controls the SVG icon tint on primary buttons. Restart Flux after adding a theme. Corner radius and border thickness are configurable in **Settings → Appearance**.

Flux uses the system sans-serif font resolved through Fontconfig. Change it in **Settings → Appearance**; **System default** follows your desktop font.

## Window manager rules

Flux uses the stable window identifier `flux` (`app_id` on Wayland and `WM_CLASS` on X11). Its desktop entry also declares `StartupWMClass=flux`.

Without `--force`, Flux exits with an error on unsupported distributions.

## Versioning and releases

The root [`VERSION`](VERSION) file is the single source of truth, using `MAJOR.MINOR.PATCH` format (for example, `0.1.1`).

The version is used by CMake, the application help output, and package metadata and filenames. The application menu displays only `Flux`.

To prepare a release:

1. Update `VERSION`.
2. Configure and build the project.
3. Run `just tag` to create the matching annotated `v<VERSION>` Git tag locally.
4. Run `just package` to create a versioned `.tar.gz` archive.

On Debian-based build systems, create a `.deb` package with:

```sh
cpack --config build/Makefiles/Release/CPackConfig.cmake -G DEB
```

Push the tag and package files through your usual release process.

## Development helpers

| Command | Description |
|---|---|
| `just build` | Build Release |
| `just build Debug` | Build Debug |
| `just run` | Run Flux |
| `just package` | Create a `.tar.gz` archive |
| `just nix` | Build with Nix |
| `just pkg` | Build an Arch package |
| `just rpm` | Build an RPM |
| `just repo` | Build the pacman repository |
| `just serve` | Serve the repository on port 8080 |
| `just fmt` | Format C++ with clang-format |
| `just tidy` | Run clang-tidy |
| `just test` | Run CTest |
| `just clean` | Remove build artifacts |
| `just tag` | Create the `v<VERSION>` Git tag |

## License

Flux is licensed under the GNU General Public License v3. See [`LICENSE`](LICENSE).