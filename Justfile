set shell := ["bash", "-euo", "pipefail", "-c"]

build_type := env("BUILD_TYPE", "Release")
cxx := env("CXX", "clang++")

# Build the default configuration (Release unless BUILD_TYPE is set).
default: build

# Configure and build. Usage: just build [Debug|Release|...]
build type=build_type: (configure type)
    cmake --build build/Makefiles/{{type}} --parallel

# Run the already-built program; this does not build it.
run:
    ./build/Makefiles/Release/bin/flux

# Build and install the package through Nix (requires flakes).
nix:
    nix build .#flux --print-out-paths

# Build the Arch Linux package with makepkg.
pkg:
    cd packaging/arch && makepkg -f

# Build a Fedora-compatible RPM with CPack (requires rpm-build).
rpm type=build_type:
    cmake -S . -B build/Makefiles/{{type}} -G "Unix Makefiles" -DCMAKE_BUILD_TYPE={{type}} -DCMAKE_CXX_COMPILER={{cxx}} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build/Makefiles/{{type}} --parallel
    mkdir -p packaging/rpm
    cpack -G RPM --config build/Makefiles/{{type}}/CPackConfig.cmake -B packaging/rpm

# Build the Arch package and refresh the local pacman repository in repo/.
repo: pkg
    mkdir -p repo
    cp packaging/arch/*.pkg.tar.zst repo/
    repo-add repo/flux.db.tar.gz repo/flux-*.pkg.tar.zst
    rm -f repo/*.old repo/flux.db repo/flux.files
    cp repo/flux.db.tar.gz repo/flux.db
    cp repo/flux.files.tar.gz repo/flux.files

# Serve repo/ over HTTP on port 8080.
serve:
    python -m http.server 8080 --directory repo

# Create an annotated release tag from VERSION (for example, v0.1.0).
tag:
    @version="$(cat VERSION)"; \
    if git rev-parse -q --verify "refs/tags/v$$version" >/dev/null; then \
        echo "Tag v$$version already exists" >&2; exit 1; \
    fi; \
    git tag -a "v$$version" -m "Flux v$$version"

# Build a versioned CPack archive from the selected build configuration.
package type=build_type: (build type)
    cpack --config build/Makefiles/{{type}}/CPackConfig.cmake

# Build and run the test suite with CTest.
test type=build_type: (build type)
    ctest --test-dir build/Makefiles/{{type}} --output-on-failure

# Rebuild from scratch.
rebuild type=build_type: clean (build type)

# Remove the Unix Makefiles build artefacts.
clean:
    rm -rf build

# Format src/ with clang-format.
fmt:
    find src -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.hpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format -i

# Run clang-tidy over src/ using the compile database.
tidy type=build_type: (configure type)
    find src -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) -print0 | xargs -0 -r clang-tidy -p build/Makefiles/{{type}}

# Show the effective build settings.
info:
    @echo "build type : {{build_type}}"
    @echo "compiler   : {{cxx}}"
    @echo "binary     : build/Makefiles/{{build_type}}/bin/flux"

[private]
configure type:
    cmake -S . -B build/Makefiles/{{type}} -G "Unix Makefiles" -DCMAKE_BUILD_TYPE={{type}} -DCMAKE_CXX_COMPILER={{cxx}} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
