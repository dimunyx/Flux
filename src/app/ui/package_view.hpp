#pragma once

#include <memory>

namespace core {
class Window;
}

namespace render {
class Renderer;
}

namespace ui {

class PackageView {
 public:
  PackageView(core::Window& window, render::Renderer& renderer, bool pacmanSupported);
  ~PackageView();

  PackageView(const PackageView&) = delete;
  PackageView& operator=(const PackageView&) = delete;

  void update(int framebufferWidth, int framebufferHeight);
  void draw(int width, int height) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ui
