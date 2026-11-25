# Flux

Flux is a lightweight Linux software center with a native desktop interface. It brings package search and common package actions into one small application, with a focus on a clean, responsive UI.

> **Current support:** package search, installation, removal, and cache clearing are implemented for Arch Linux with `pacman`. The interface is designed to be independent of a desktop environment, but package-manager support for other distributions is not implemented yet.

## Features

- Search Arch Linux repositories and see package descriptions and installed status.
- Install or remove packages through Polkit authentication.
- Clear the pacman package cache after confirmation.
- Choose the interface theme and font, adjust font size, and control animation and scrolling behavior.
- Use keyboard navigation and a searchable, animated interface.

## Requirements

- Linux with OpenGL 4.6 support.
- CMake 3.28 or newer and a C++26 compiler. Clang is recommended; GCC is also supported.
- OpenGL development files, GLFW 3.3+, FreeType, Fontconfig, libpng, libepoxy, and pkg-config.
- For package operations on Arch Linux: `pacman` and Polkit (`pkexec`).
- Optional: [`just`](https://just.systems/) for the helper commands below.
- Optional: [Nix](https://nixos.org/) with flakes enabled, to build or install the app without managing the dependencies above.

On Arch Linux, the build dependencies can be installed with:

```sh
sudo pacman -S --needed base-devel cmake clang glfw-x11 freetype2 fontconfig libpng libepoxy pkgconf
```

The GLFW package name may differ if you use a different window-system backend.

Flux renders text with the system sans-serif font resolved through Fontconfig, the same mechanism GTK and Qt use. The font can be changed in **Settings → Appearance**, where **System default** follows the desktop font; until a font is chosen, the system font is used.

## Build and run

With `just`:

```sh
just build
just run
```

Or with CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/flux
```

The executable prints its embedded version with:

```sh
./build/bin/flux --version
```

`--force` starts the interface without package management, regardless of the detected distribution:

```sh
./build/bin/flux --force
```

Without `--force`, Flux exits with an error on a distribution whose package manager is not supported.

The default build uses Release mode. Set `CXX` to select a compiler, for example `CXX=g++ just build`. Set `BUILD_TYPE=Debug` to build a debug configuration with `just`.

## Install with Nix

The repository is a Nix flake, so Nix can build and install the application itself and all of its build dependencies:

```sh
# Build the package and keep it in the Nix store.
nix build .

# Run without installing.
nix run github:dimunyx/Flux

# Install the app into your Nix profile.
nix profile install github:dimunyx/Flux
```

From a local checkout, use `nix build .`, `nix run .`, or `nix profile install .` instead. The flake supports `x86_64-linux` and `aarch64-linux`.

The package is built with Clang, ThinLTO, and without `-march=native`, so the result is reproducible and can be built by a remote builder. The first `nix build` creates a `flake.lock` file; commit it to pin the exact `nixpkgs` revision.

A development shell with the compiler and all libraries is also available:

```sh
nix develop
just build
```

> Flux's package management actions still call `/usr/bin/pacman` and `/usr/bin/pkexec` directly, so they work when Flux is installed with Nix on Arch Linux. On NixOS the interface starts, but installing and removing packages is unavailable.

## Arch Linux package and local repository

`packaging/arch/PKGBUILD` builds a native pacman package from the current checkout:

```sh
just pkg
```

This runs `makepkg` in `packaging/arch` and produces `flux-<version>-1-<arch>.pkg.tar.zst` there. To build, install missing build dependencies, and install the package in one step:

```sh
cd packaging/arch
makepkg -si
```

To turn the package into a pacman repository inside this repository, build the package and add it to `repo/`:

```sh
just repo
```

Or do the same manually:

```sh
mkdir -p repo
cp packaging/arch/*.pkg.tar.zst repo/
repo-add repo/flux.db.tar.gz repo/flux-*.pkg.tar.zst
```

`repo-add` writes `flux.db.tar.gz` and `flux.files.tar.gz` (plus the `flux.db` and `flux.files` symlinks) next to the package. Point pacman at the directory with a `file://` server:

```ini
[flux]
SigLevel = Optional TrustAll
Server = file:///home/you/dev/Flux/repo
```

Or serve `repo/` over HTTP and use that URL instead:

```sh
just serve   # python -m http.server 8080 --directory repo
```

```ini
[flux]
SigLevel = Optional TrustAll
Server = http://localhost:8080
```

Then refresh the databases and install:

```sh
sudo pacman -Syu flux
```

The `PKGBUILD` sets `-DFLUX_NATIVE=OFF` so the package does not depend on the CPU it was built on. Packages are unsigned; `SigLevel = Optional TrustAll` is fine for a local or test repository. For a public repository, sign the database and packages with GPG and require signatures in `pacman.conf`.

To publish the repository from this Git repository, upload the contents of `repo/` to a static host or a `gh-pages` branch and use the published URL as `Server`. The `repo/` directory is a build artifact and is ignored by Git.

## One version for the app, tags, and packages

The root [`VERSION`](VERSION) file is the single source of truth. It contains one semantic version in `MAJOR.MINOR.PATCH` form, such as `0.1.0`.

Changing `VERSION` updates the CMake project version. CMake embeds that version in the executable, so `flux --version` and the window title show the same value. CPack also uses the CMake project version for generated package metadata and filenames. The install package includes the executable and its image assets.

To prepare a release:

1. Set the new version in `VERSION`, for example `0.2.0`.
2. Configure and build from the updated source.
3. Create a matching annotated Git tag with `just tag`. This creates `v0.2.0` from the contents of `VERSION`; it does not push the tag.
4. Build a distributable archive with `just package`. This creates a versioned `.tar.gz` package. To create a Debian package on a Debian-based build system, run `cpack --config build/Makefiles/Release/CPackConfig.cmake -G DEB`.

The tag is created locally and the package version is read through CMake from `VERSION`; there is no separate version number to edit in a package manifest. Before publishing a release, push the tag and the package files using your normal release process.

## Development helpers

```sh
just build       # Configure and build Release
just build Debug # Configure and build Debug
just package     # Build a versioned .tar.gz package
just run         # Run the Release executable
just nix         # Build the package with Nix (requires flakes)
just pkg         # Build the Arch Linux package with makepkg
just repo        # Build the package and refresh repo/ (pacman repository)
just serve       # Serve repo/ over HTTP on port 8080
just fmt         # Format C++ source with clang-format
just tidy        # Run clang-tidy
just clean       # Remove Makefiles build artifacts
just tag         # Create the v<VERSION> annotated Git tag
```

`just test` runs CTest. Tests can be added to the project with CMake/CTest as development continues.

## License

Flux is licensed under the GNU General Public License, version 3. See [`LICENSE`](LICENSE).