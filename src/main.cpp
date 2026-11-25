#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "main.hpp"

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "Flux " << FLUX_VERSION << '\n';
    return 0;
  }

  try {
    std::string distro;
    const bool osDetected = getOS(distro) == 0;
    if (osDetected) {
      static_cast<void>(getPM(distro));
    }
    const bool pacmanSupported = osDetected && distro == "arch";

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
