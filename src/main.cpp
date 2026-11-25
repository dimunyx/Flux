#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "main.hpp"

int main(int argc, char* argv[]) {
  bool force = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--version") {
      std::cout << "Flux " << FLUX_VERSION << '\n';
      return 0;
    }
    if (argument == "--force") {
      force = true;
    }
  }

  try {
    // --force starts the interface without package management, regardless of
    // the distribution. Without it, an unsupported distribution is an error.
    bool pacmanSupported = false;
    if (!force) {
      std::string distro;
      if (getOS(distro) != 0 || getPM(distro) != 0) {
        std::cerr << "Flux: unsupported distribution. "
                     "Start with --force to run without package management.\n";
        return 1;
      }
      pacmanSupported = distro == "arch";
    }

    core::Window window(1280, 720, "Flux " FLUX_VERSION);
    render::Renderer renderer;
    ui::PackageView packageView(window, renderer, pacmanSupported);

    while (!window.shouldClose()) {
      window.pollEvents();

      int width = 0;
      int height = 0;
      window.getFramebufferSize(width, height);
      packageView.update(width, height);
      packageView.draw(width, height);

      window.swapBuffers();
    }
  } catch (const std::exception& error) {
    std::cerr << "Flux: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
