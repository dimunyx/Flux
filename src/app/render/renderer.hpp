#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace render {

struct TextItem {
  std::string text;
  float x = 0.0F;
  float baseline = 0.0F;
  std::array<float, 3> color{0.78F, 0.88F, 1.0F};
  float opacity = 1.0F;
};

struct ImageItem {
  std::string path;
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  float opacity = 1.0F;
  bool circular = false;
  std::array<float, 3> tint{1.0F, 1.0F, 1.0F};
  bool themed = false;
};

struct FontFace {
  std::string path;
  std::string family;
  std::string style;
};

struct RectItem {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  std::array<float, 3> color{0.0F, 0.0F, 0.0F};
  float radius = 0.0F;
  float opacity = 1.0F;
};

struct ClipRect {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  int fontSize() const;
  float baselineForCenter(float centerY) const;
  void setFontSize(int size);
  std::string fontFacePath() const;
  std::string fontFamily() const;
  std::string fontStyle() const;
  std::vector<FontFace> fontFaces() const;
  void setFontFace(const std::string& path);
  void useSystemFont();
  float measureText(std::string_view text) const;
  void draw(int width, int height, std::span<const RectItem> rectangles,
            std::span<const TextItem> text,
            std::span<const ImageItem> images = {}) const;
  void drawClipped(int width, int height, const ClipRect& clip,
                   std::span<const RectItem> rectangles,
                   std::span<const TextItem> text,
                   std::span<const ImageItem> images = {}) const;
  void blurCurrentFramebuffer(int width, int height, float amount) const;
  void drawOverlay(int width, int height, std::span<const RectItem> rectangles,
                   std::span<const TextItem> text,
                   std::span<const ImageItem> images = {}) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
