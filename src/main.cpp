#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "i18n/catalog.hpp"
#include "main.hpp"

int main(int argc, char* argv[]) {
  const i18n::Catalog catalog = i18n::Catalog::fromSystemLocale();
  bool force = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      std::cout << catalog.format("help.version", {{"version", FLUX_VERSION}})
                << '\n'
                << catalog.text("help.usage") << "\n\n"
                << catalog.text("help.options") << '\n'
                << "  -h, --help  " << catalog.text("help.help") << '\n'
                << "  --force     " << catalog.text("help.force") << '\n';
      return 0;
    }
    if (argument == "--force") {
      force = true;
    }
  }

  try {
    // --force starts the interface without package management, regardless of
    // the distribution. Without it, an unsupported distribution is an error.
    pm::Manager packageManager = pm::Manager::none;
    if (!force) {
      OSInfo os;
      if (getOS(os) != 0 ||
          (packageManager = pm::getPM(os.id, os.idLike)) == pm::Manager::none) {
        std::cerr << catalog.text("app.unsupported") << '\n';
        return 1;
      }
    }

    core::Window window(1280, 720, "Flux");
    render::Renderer renderer;
    ui::PackageView packageView(window, renderer, packageManager);

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
    std::cerr << catalog.format("app.error", {{"error", error.what()}}) << '\n';
    return 1;
  }

  return 0;
}
