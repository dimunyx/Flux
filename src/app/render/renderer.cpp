#include "render/renderer.hpp"

#include <epoxy/gl.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <png.h>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace render {

namespace {

constexpr int kMinimumFontSize = 12;
constexpr int kMaximumFontSize = 30;
constexpr int kDefaultFontSize = 18;

constexpr char kVertexShader[] = R"(
  #version 460 core
  layout (location = 0) in vec4 vertex;
  out vec2 texCoords;
  uniform vec2 viewport;

  void main() {
    vec2 position = vec2(
      vertex.x / viewport.x * 2.0 - 1.0,
      1.0 - vertex.y / viewport.y * 2.0
    );
    gl_Position = vec4(position, 0.0, 1.0);
    texCoords = vertex.zw;
  }
)";

constexpr char kFragmentShader[] = R"(
  #version 460 core
  in vec2 texCoords;
  out vec4 fragmentColor;
  uniform sampler2D glyphTexture;
  uniform vec3 textColor;
  uniform float textOpacity;

  void main() {
    float alpha = texture(glyphTexture, texCoords).r;
    fragmentColor = vec4(textColor, alpha * textOpacity);
  }
)";

constexpr char kImageFragmentShader[] = R"(
  #version 460 core
  in vec2 texCoords;
  out vec4 fragmentColor;
  uniform sampler2D imageTexture;
  uniform float imageOpacity;
  uniform float imageCircular;

  void main() {
    if (imageCircular > 0.5 && distance(texCoords, vec2(0.5)) > 0.5) {
      discard;
    }
    vec4 color = texture(imageTexture, texCoords);
    color.a *= imageOpacity;
    fragmentColor = color;
  }
)";

constexpr char kRectangleVertexShader[] = R"(
  #version 460 core
  layout (location = 0) in vec4 vertex;
  out vec2 localPosition;
  uniform vec2 viewport;

  void main() {
    vec2 position = vec2(
      vertex.x / viewport.x * 2.0 - 1.0,
      1.0 - vertex.y / viewport.y * 2.0
    );
    gl_Position = vec4(position, 0.0, 1.0);
    localPosition = vertex.zw;
  }
)";

constexpr char kRectangleFragmentShader[] = R"(
  #version 460 core
  in vec2 localPosition;
  out vec4 fragmentColor;
  uniform vec2 rectangleSize;
  uniform float cornerRadius;
  uniform vec3 rectangleColor;
  uniform float rectangleOpacity;

  void main() {
    vec2 halfSize = rectangleSize * 0.5;
    float radius = clamp(cornerRadius, 0.0, min(halfSize.x, halfSize.y));
    vec2 corner = abs(localPosition - halfSize) - (halfSize - vec2(radius));
    float signedDistance = length(max(corner, vec2(0.0))) +
                           min(max(corner.x, corner.y), 0.0) - radius;
    float edgeWidth = max(fwidth(signedDistance), 0.5);
    float alpha = 1.0 - smoothstep(-edgeWidth, edgeWidth, signedDistance);
    fragmentColor = vec4(rectangleColor, alpha * rectangleOpacity);
  }
)";

constexpr char kBlurVertexShader[] = R"(
  #version 460 core
  layout (location = 0) in vec4 vertex;
  out vec2 texCoords;
  uniform vec2 viewport;

  void main() {
    vec2 position = vec2(
      vertex.x / viewport.x * 2.0 - 1.0,
      1.0 - vertex.y / viewport.y * 2.0
    );
    gl_Position = vec4(position, 0.0, 1.0);
    texCoords = vertex.zw;
  }
)";

constexpr char kBlurFragmentShader[] = R"(
  #version 460 core
  in vec2 texCoords;
  out vec4 fragmentColor;
  uniform sampler2D sourceTexture;
  uniform vec2 texelSize;
  uniform float blurAmount;

  void main() {
    vec2 stepSize = texelSize * 3.5;
    vec3 center = texture(sourceTexture, texCoords).rgb * 0.25;
    vec3 axial =
        (texture(sourceTexture, texCoords + vec2(stepSize.x, 0.0)).rgb +
         texture(sourceTexture, texCoords - vec2(stepSize.x, 0.0)).rgb +
         texture(sourceTexture, texCoords + vec2(0.0, stepSize.y)).rgb +
         texture(sourceTexture, texCoords - vec2(0.0, stepSize.y)).rgb) * 0.125;
    vec3 diagonal =
        (texture(sourceTexture, texCoords + stepSize).rgb +
         texture(sourceTexture, texCoords - stepSize).rgb +
         texture(sourceTexture, texCoords + vec2(stepSize.x, -stepSize.y)).rgb +
         texture(sourceTexture, texCoords + vec2(-stepSize.x, stepSize.y)).rgb) * 0.0625;
    vec3 blurred = center + axial + diagonal;
    vec3 original = texture(sourceTexture, texCoords).rgb;
    fragmentColor = vec4(mix(original, blurred, clamp(blurAmount, 0.0, 1.0)), 1.0);
  }
)";

struct DecodedCodepoint {
  char32_t value;
  std::size_t length;
};

DecodedCodepoint decodeCodepoint(std::string_view text, std::size_t offset) {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80) {
    return {first, 1};
  }

  std::size_t length = 0;
  char32_t value = 0;
  if ((first & 0xE0) == 0xC0) {
    length = 2;
    value = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    value = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    value = first & 0x07;
  } else {
    return {U'\uFFFD', 1};
  }

  if (offset + length > text.size()) {
    return {U'\uFFFD', 1};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0) != 0x80) {
      return {U'\uFFFD', 1};
    }
    value = (value << 6) | (continuation & 0x3F);
  }
  if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
      (length == 4 && value < 0x10000) || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return {U'\uFFFD', 1};
  }
  return {value, length};
}

std::string shaderLog(GLuint shader) {
  GLint length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
  if (length <= 1) {
    return {};
  }

  std::string log(static_cast<std::size_t>(length), '\0');
  GLsizei written = 0;
  glGetShaderInfoLog(shader, length, &written, log.data());
  log.resize(static_cast<std::size_t>(written));
  return log;
}

GLuint compileShader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  if (shader == 0) {
    throw std::runtime_error("Failed to create a shader");
  }

  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_FALSE) {
    const std::string log = shaderLog(shader);
    glDeleteShader(shader);
    throw std::runtime_error("Failed to compile shader: " + log);
  }
  return shader;
}

GLuint createProgram(const char* vertexSource, const char* fragmentSource) {
  const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
  GLuint fragmentShader = 0;
  try {
    fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
  } catch (...) {
    glDeleteShader(vertexShader);
    throw;
  }

  const GLuint program = glCreateProgram();
  if (program == 0) {
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    throw std::runtime_error("Failed to create a shader program");
  }

  glAttachShader(program, vertexShader);
  glAttachShader(program, fragmentShader);
  glLinkProgram(program);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log;
    if (length > 1) {
      log.resize(static_cast<std::size_t>(length));
      GLsizei written = 0;
      glGetProgramInfoLog(program, length, &written, log.data());
      log.resize(static_cast<std::size_t>(written));
    }
    glDeleteProgram(program);
    throw std::runtime_error("Failed to link shader program: " + log);
  }
  return program;
}

}  // namespace

struct Renderer::Impl {
  struct Character {
    GLuint texture = 0;
    FT_UInt glyphIndex = 0;
    int width = 0;
    int height = 0;
    int bearingX = 0;
    int bearingY = 0;
    float advance = 0.0F;
  };

  struct ImageTexture {
    GLuint texture = 0;
    int width = 0;
    int height = 0;
  };

  FT_Library library = nullptr;
  FT_Face face = nullptr;
  GLuint program = 0;
  GLuint imageProgram = 0;
  GLuint rectangleProgram = 0;
  GLuint blurProgram = 0;
  mutable GLuint blurTexture = 0;
  GLuint vertexArray = 0;
  GLuint vertexBuffer = 0;
  GLint viewportLocation = -1;
  GLint colorLocation = -1;
  GLint textureLocation = -1;
  GLint textOpacityLocation = -1;
  GLint imageViewportLocation = -1;
  GLint imageTextureLocation = -1;
  GLint imageOpacityLocation = -1;
  GLint imageCircularLocation = -1;
  GLint rectangleViewportLocation = -1;
  GLint rectangleSizeLocation = -1;
  GLint rectangleRadiusLocation = -1;
  GLint rectangleColorLocation = -1;
  GLint rectangleOpacityLocation = -1;
  GLint blurViewportLocation = -1;
  GLint blurTextureLocation = -1;
  GLint blurTexelSizeLocation = -1;
  GLint blurAmountLocation = -1;
  int currentFontSize = 0;
  mutable int blurTextureWidth = 0;
  mutable int blurTextureHeight = 0;
  std::string currentFontPath;
  std::string currentFontFamily;
  std::string currentFontStyle;
  mutable std::unordered_map<char32_t, Character> characters;
  mutable std::unordered_map<std::string, ImageTexture> imageTextures;
  mutable std::vector<FontFace> availableFontFaces;
  mutable bool fontFacesDiscovered = false;

  Impl() {
    try {
      loadFont();
      createGraphicsResources();
    } catch (...) {
      cleanup();
      throw;
    }
  }

  ~Impl() {
    cleanup();
  }

  static std::string systemFontPath() {
    if (FcInit() == 0) {
      return {};
    }

    FcPattern* pattern = FcPatternCreate();
    if (pattern == nullptr) {
      return {};
    }
    FcPatternAddString(pattern, FC_FAMILY,
                       reinterpret_cast<const FcChar8*>("sans-serif"));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    std::string path;
    if (match != nullptr) {
      FcChar8* file = nullptr;
      if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        path = reinterpret_cast<const char*>(file);
      }
      FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    return path;
  }

  void loadFont() {
    if (FT_Init_FreeType(&library) != 0) {
      throw std::runtime_error("Failed to initialize FreeType");
    }

    // Resolve the desktop's configured sans-serif font through fontconfig,
    // which is what GTK and Qt use for system fonts.
    const std::string path = systemFontPath();
    if (path.empty() || FT_New_Face(library, path.c_str(), 0, &face) != 0) {
      throw std::runtime_error(
          "Could not load a system font; install fontconfig and a font package");
    }

    currentFontPath = path;
    currentFontFamily = face->family_name != nullptr ? face->family_name : "Unknown";
    currentFontStyle = face->style_name != nullptr ? face->style_name : "Regular";
    setFontSize(kDefaultFontSize);
  }

  Character rasterizeCharacter(char32_t codepoint) const {
    FT_ULong loadedCodepoint = static_cast<FT_ULong>(codepoint);
    constexpr FT_Int32 loadFlags = FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT;
    if (FT_Load_Char(face, loadedCodepoint, loadFlags) != 0) {
      loadedCodepoint = static_cast<FT_ULong>(U'?');
      if (FT_Load_Char(face, loadedCodepoint, loadFlags) != 0) {
        return {};
      }
    }

    const FT_Bitmap& bitmap = face->glyph->bitmap;
    Character character{
        .glyphIndex = FT_Get_Char_Index(face, loadedCodepoint),
        .width = static_cast<int>(bitmap.width),
        .height = static_cast<int>(bitmap.rows),
        .bearingX = face->glyph->bitmap_left,
        .bearingY = face->glyph->bitmap_top,
        .advance = static_cast<float>(face->glyph->advance.x) / 64.0F};

    if (bitmap.width <= 0 || bitmap.rows <= 0) {
      return character;
    }

    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(bitmap.width) * bitmap.rows);
    for (int row = 0; row < static_cast<int>(bitmap.rows); ++row) {
      const int sourceRow = bitmap.pitch >= 0
                                ? row
                                : static_cast<int>(bitmap.rows) - 1 - row;
      const unsigned char* source =
          bitmap.buffer + static_cast<std::ptrdiff_t>(sourceRow) * bitmap.pitch;
      unsigned char* destination =
          pixels.data() + static_cast<std::size_t>(row) * bitmap.width;
      for (int column = 0; column < static_cast<int>(bitmap.width); ++column) {
        if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
          const unsigned int sample = source[column];
          const unsigned int levels =
              std::max(2U, static_cast<unsigned int>(bitmap.num_grays));
          destination[column] = static_cast<unsigned char>(
              std::min(255U, sample * 255U / (levels - 1U)));
        } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
          const unsigned char mask = static_cast<unsigned char>(0x80U >> (column % 8));
          destination[column] = (source[column / 8] & mask) != 0 ? 255 : 0;
        } else if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
          destination[column] = source[column * 4 + 3];
        } else {
          return {};
        }
      }
    }

    glGenTextures(1, &character.texture);
    glBindTexture(GL_TEXTURE_2D, character.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, character.width, character.height, 0,
                 GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return character;
  }

  void clearCharacters() {
    for (const auto& [codepoint, character] : characters) {
      static_cast<void>(codepoint);
      if (character.texture != 0) {
        glDeleteTextures(1, &character.texture);
      }
    }
    characters.clear();
  }

  void loadBasicCharacters() {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (char32_t codepoint = 0; codepoint < 128; ++codepoint) {
      characters.emplace(codepoint, rasterizeCharacter(codepoint));
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  Character& characterForCodepoint(char32_t codepoint) const {
    const auto found = characters.find(codepoint);
    if (found != characters.end()) {
      return found->second;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    Character character = rasterizeCharacter(codepoint);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return characters.emplace(codepoint, character).first->second;
  }

  ImageTexture loadImageTexture(const std::string& path) const {
    struct ImageGuard {
      png_image image{};
      ~ImageGuard() { png_image_free(&image); }
    };

    ImageGuard guard;
    png_image& pngImage = guard.image;
    pngImage.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&pngImage, path.c_str()) == 0) {
      const std::string error = pngImage.message;
      throw std::runtime_error("Could not read image asset '" + path + "': " + error);
    }

    if (pngImage.width == 0 || pngImage.height == 0 || pngImage.width > 4096 ||
        pngImage.height > 4096) {
      throw std::runtime_error("Image asset has invalid dimensions: " + path);
    }

    const int imageWidth = static_cast<int>(pngImage.width);
    const int imageHeight = static_cast<int>(pngImage.height);
    pngImage.format = PNG_FORMAT_RGBA;
    std::vector<png_byte> pixels(
        static_cast<std::size_t>(PNG_IMAGE_SIZE(pngImage)));
    if (png_image_finish_read(&pngImage, nullptr, pixels.data(), 0, nullptr) == 0) {
      const std::string error = pngImage.message;
      throw std::runtime_error("Could not decode image asset '" + path + "': " + error);
    }

    ImageTexture result{
        .width = imageWidth,
        .height = imageHeight};
    glGenTextures(1, &result.texture);
    if (result.texture == 0) {
      throw std::runtime_error("Could not create texture for image asset: " + path);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, result.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, result.width, result.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return result;
  }

  void setFontSize(int size) {
    if (size < kMinimumFontSize || size > kMaximumFontSize) {
      throw std::invalid_argument("Font size must be between 12 and 30 pixels");
    }
    if (size == currentFontSize) {
      return;
    }
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size)) != 0) {
      throw std::runtime_error("Failed to set the font size");
    }
    clearCharacters();
    loadBasicCharacters();
    currentFontSize = size;
  }

  void setFontFace(const std::string& path) {
    if (path == currentFontPath) {
      return;
    }
    FT_Face newFace = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &newFace) != 0) {
      throw std::runtime_error("Could not load font face: " + path);
    }
    if (FT_Set_Pixel_Sizes(newFace, 0, static_cast<FT_UInt>(currentFontSize)) != 0) {
      FT_Done_Face(newFace);
      throw std::runtime_error("Could not set the selected font size");
    }

    FT_Face oldFace = face;
    face = newFace;
    currentFontPath = path;
    currentFontFamily = face->family_name != nullptr ? face->family_name : "Unknown";
    currentFontStyle = face->style_name != nullptr ? face->style_name : "Regular";
    clearCharacters();
    loadBasicCharacters();
    FT_Done_Face(oldFace);
  }

  std::vector<FontFace> fontFaces() const {
    if (fontFacesDiscovered) {
      return availableFontFaces;
    }

    std::set<std::string> seenPaths;
    availableFontFaces.clear();
    if (FcInit() != 0) {
      FcPattern* pattern = FcPatternCreate();
      FcObjectSet* objects =
          FcObjectSetBuild(FC_FILE, FC_FAMILY, FC_STYLE, nullptr);
      FcFontSet* fontSet =
          pattern != nullptr && objects != nullptr
              ? FcFontList(nullptr, pattern, objects)
              : nullptr;
      if (fontSet != nullptr) {
        for (int index = 0; index < fontSet->nfont; ++index) {
          FcPattern* font = fontSet->fonts[index];
          FcChar8* file = nullptr;
          FcChar8* family = nullptr;
          FcChar8* style = nullptr;
          if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch ||
              FcPatternGetString(font, FC_FAMILY, 0, &family) != FcResultMatch) {
            continue;
          }
          const std::string path = reinterpret_cast<const char*>(file);
          const std::string extension =
              std::filesystem::path(path).extension().string();
          if (extension != ".ttf" && extension != ".otf" &&
              extension != ".ttc" && extension != ".TTF" &&
              extension != ".OTF" && extension != ".TTC") {
            continue;
          }
          if (!seenPaths.insert(path).second) {
            continue;
          }
          const std::string styleName =
              FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch
                  ? reinterpret_cast<const char*>(style)
                  : "Regular";
          availableFontFaces.push_back(
              {path, reinterpret_cast<const char*>(family), styleName});
        }
        FcFontSetDestroy(fontSet);
      }
      if (objects != nullptr) {
        FcObjectSetDestroy(objects);
      }
      if (pattern != nullptr) {
        FcPatternDestroy(pattern);
      }
    }
    if (!currentFontPath.empty() && seenPaths.insert(currentFontPath).second) {
      availableFontFaces.push_back(
          {currentFontPath, currentFontFamily, currentFontStyle});
    }
    std::sort(availableFontFaces.begin(), availableFontFaces.end(),
              [](const FontFace& left, const FontFace& right) {
                if (left.family != right.family) {
                  return left.family < right.family;
                }
                if (left.style != right.style) {
                  return left.style < right.style;
                }
                return left.path < right.path;
              });
    fontFacesDiscovered = true;
    return availableFontFaces;
  }

  float measureText(std::string_view text) const {
    float width = 0.0F;
    FT_UInt previousGlyph = 0;
    for (std::size_t offset = 0; offset < text.size();) {
      const DecodedCodepoint decoded = decodeCodepoint(text, offset);
      const Character& character = characterForCodepoint(decoded.value);
      width += kerning(previousGlyph, character.glyphIndex) + character.advance;
      previousGlyph = character.glyphIndex;
      offset += decoded.length;
    }
    return width;
  }

  float kerning(FT_UInt previousGlyph, FT_UInt currentGlyph) const {
    if (previousGlyph == 0 || currentGlyph == 0 || !FT_HAS_KERNING(face)) {
      return 0.0F;
    }
    FT_Vector adjustment{};
    if (FT_Get_Kerning(face, previousGlyph, currentGlyph, FT_KERNING_DEFAULT,
                       &adjustment) != 0) {
      return 0.0F;
    }
    return static_cast<float>(adjustment.x) / 64.0F;
  }

  void createGraphicsResources() {
    program = createProgram(kVertexShader, kFragmentShader);
    viewportLocation = glGetUniformLocation(program, "viewport");
    colorLocation = glGetUniformLocation(program, "textColor");
    textureLocation = glGetUniformLocation(program, "glyphTexture");
    textOpacityLocation = glGetUniformLocation(program, "textOpacity");
    if (viewportLocation < 0 || colorLocation < 0 || textureLocation < 0 ||
        textOpacityLocation < 0) {
      throw std::runtime_error("Failed to locate text shader uniforms");
    }

    imageProgram = createProgram(kVertexShader, kImageFragmentShader);
    imageViewportLocation = glGetUniformLocation(imageProgram, "viewport");
    imageTextureLocation = glGetUniformLocation(imageProgram, "imageTexture");
    imageOpacityLocation = glGetUniformLocation(imageProgram, "imageOpacity");
    imageCircularLocation = glGetUniformLocation(imageProgram, "imageCircular");
    if (imageViewportLocation < 0 || imageTextureLocation < 0 ||
        imageOpacityLocation < 0 || imageCircularLocation < 0) {
      throw std::runtime_error("Failed to locate image shader uniforms");
    }

    rectangleProgram =
        createProgram(kRectangleVertexShader, kRectangleFragmentShader);
    rectangleViewportLocation = glGetUniformLocation(rectangleProgram, "viewport");
    rectangleSizeLocation = glGetUniformLocation(rectangleProgram, "rectangleSize");
    rectangleRadiusLocation = glGetUniformLocation(rectangleProgram, "cornerRadius");
    rectangleColorLocation = glGetUniformLocation(rectangleProgram, "rectangleColor");
    rectangleOpacityLocation =
        glGetUniformLocation(rectangleProgram, "rectangleOpacity");
    if (rectangleViewportLocation < 0 || rectangleSizeLocation < 0 ||
        rectangleRadiusLocation < 0 || rectangleColorLocation < 0 ||
        rectangleOpacityLocation < 0) {
      throw std::runtime_error("Failed to locate rectangle shader uniforms");
    }

    blurProgram = createProgram(kBlurVertexShader, kBlurFragmentShader);
    blurViewportLocation = glGetUniformLocation(blurProgram, "viewport");
    blurTextureLocation = glGetUniformLocation(blurProgram, "sourceTexture");
    blurTexelSizeLocation = glGetUniformLocation(blurProgram, "texelSize");
    blurAmountLocation = glGetUniformLocation(blurProgram, "blurAmount");
    if (blurViewportLocation < 0 || blurTextureLocation < 0 ||
        blurTexelSizeLocation < 0 || blurAmountLocation < 0) {
      throw std::runtime_error("Failed to locate blur shader uniforms");
    }

    glGenVertexArrays(1, &vertexArray);
    glGenBuffers(1, &vertexBuffer);
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
  }

  void cleanup() {
    clearCharacters();
    for (const auto& [path, image] : imageTextures) {
      static_cast<void>(path);
      if (image.texture != 0) {
        glDeleteTextures(1, &image.texture);
      }
    }
    imageTextures.clear();
    if (blurTexture != 0) {
      glDeleteTextures(1, &blurTexture);
      blurTexture = 0;
    }
    characters.clear();
    if (vertexBuffer != 0) {
      glDeleteBuffers(1, &vertexBuffer);
      vertexBuffer = 0;
    }
    if (vertexArray != 0) {
      glDeleteVertexArrays(1, &vertexArray);
      vertexArray = 0;
    }
    if (program != 0) {
      glDeleteProgram(program);
      program = 0;
    }
    if (imageProgram != 0) {
      glDeleteProgram(imageProgram);
      imageProgram = 0;
    }
    if (rectangleProgram != 0) {
      glDeleteProgram(rectangleProgram);
      rectangleProgram = 0;
    }
    if (blurProgram != 0) {
      glDeleteProgram(blurProgram);
      blurProgram = 0;
    }
    if (face != nullptr) {
      FT_Done_Face(face);
      face = nullptr;
    }
    if (library != nullptr) {
      FT_Done_FreeType(library);
      library = nullptr;
    }
  }

  void drawRectangles(int width, int height,
                      std::span<const RectItem> rectangles) const {
    if (rectangles.empty()) {
      return;
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(rectangleProgram);
    glUniform2f(rectangleViewportLocation, static_cast<float>(width),
                static_cast<float>(height));
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    for (const RectItem& rectangle : rectangles) {
      if (rectangle.width <= 0.0F || rectangle.height <= 0.0F) {
        continue;
      }
      const std::array<float, 24> vertices{
          rectangle.x, rectangle.y, 0.0F, 0.0F,
          rectangle.x, rectangle.y + rectangle.height, 0.0F, rectangle.height,
          rectangle.x + rectangle.width, rectangle.y + rectangle.height,
          rectangle.width, rectangle.height,
          rectangle.x, rectangle.y, 0.0F, 0.0F,
          rectangle.x + rectangle.width, rectangle.y + rectangle.height,
          rectangle.width, rectangle.height,
          rectangle.x + rectangle.width, rectangle.y, rectangle.width, 0.0F};
      glUniform2f(rectangleSizeLocation, rectangle.width, rectangle.height);
      glUniform1f(rectangleRadiusLocation, rectangle.radius);
      glUniform3f(rectangleColorLocation, rectangle.color[0], rectangle.color[1],
                  rectangle.color[2]);
      glUniform1f(rectangleOpacityLocation,
                  std::clamp(rectangle.opacity, 0.0F, 1.0F));
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
      glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
  }

  void drawImages(int width, int height,
                  std::span<const ImageItem> images) const {
    if (images.empty()) {
      return;
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(imageProgram);
    glUniform2f(imageViewportLocation, static_cast<float>(width),
                static_cast<float>(height));
    glUniform1i(imageTextureLocation, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);

    for (const ImageItem& item : images) {
      if (item.width <= 0.0F || item.height <= 0.0F || item.opacity <= 0.0F) {
        continue;
      }
      auto texture = imageTextures.find(item.path);
      if (texture == imageTextures.end()) {
        texture = imageTextures.emplace(item.path, loadImageTexture(item.path)).first;
      }
      const std::array<float, 24> vertices{
          item.x, item.y, 0.0F, 0.0F,
          item.x, item.y + item.height, 0.0F, 1.0F,
          item.x + item.width, item.y + item.height, 1.0F, 1.0F,
          item.x, item.y, 0.0F, 0.0F,
          item.x + item.width, item.y + item.height, 1.0F, 1.0F,
          item.x + item.width, item.y, 1.0F, 0.0F};
      glUniform1f(imageOpacityLocation, std::clamp(item.opacity, 0.0F, 1.0F));
      glUniform1f(imageCircularLocation, item.circular ? 1.0F : 0.0F);
      glBindTexture(GL_TEXTURE_2D, texture->second.texture);
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
      glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
  }

  void drawText(int width, int height, std::span<const TextItem> text) const {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program);
    glUniform2f(viewportLocation, static_cast<float>(width), static_cast<float>(height));
    glUniform1i(textureLocation, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);

    for (const TextItem& item : text) {
      float cursorX = item.x;
      FT_UInt previousGlyph = 0;
      glUniform3f(colorLocation, item.color[0], item.color[1], item.color[2]);
      glUniform1f(textOpacityLocation, std::clamp(item.opacity, 0.0F, 1.0F));
      for (std::size_t offset = 0; offset < item.text.size();) {
        const DecodedCodepoint decoded = decodeCodepoint(item.text, offset);
        const Character& character = characterForCodepoint(decoded.value);
        offset += decoded.length;
        cursorX += kerning(previousGlyph, character.glyphIndex);
        const float x = cursorX + static_cast<float>(character.bearingX);
        const float y = item.baseline - static_cast<float>(character.bearingY);
        const float glyphWidth = static_cast<float>(character.width);
        const float glyphHeight = static_cast<float>(character.height);
        if (character.texture != 0) {
          const std::array<float, 24> vertices{
              x, y, 0.0F, 0.0F, x, y + glyphHeight, 0.0F, 1.0F,
              x + glyphWidth, y + glyphHeight, 1.0F, 1.0F,
              x, y, 0.0F, 0.0F, x + glyphWidth, y + glyphHeight, 1.0F, 1.0F,
              x + glyphWidth, y, 1.0F, 0.0F};
          glBindTexture(GL_TEXTURE_2D, character.texture);
          glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
          glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        cursorX += character.advance;
        previousGlyph = character.glyphIndex;
      }
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
  }

  void blurCurrentFramebuffer(int width, int height, float amount) const {
    if (width <= 0 || height <= 0 || amount <= 0.0F) {
      return;
    }
    if (blurTexture == 0) {
      glGenTextures(1, &blurTexture);
    }
    glBindTexture(GL_TEXTURE_2D, blurTexture);
    if (blurTextureWidth != width || blurTextureHeight != height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      blurTextureWidth = width;
      blurTextureHeight = height;
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    const std::array<float, 24> vertices{
        0.0F, 0.0F, 0.0F, 1.0F,
        0.0F, static_cast<float>(height), 0.0F, 0.0F,
        static_cast<float>(width), static_cast<float>(height), 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
        static_cast<float>(width), static_cast<float>(height), 1.0F, 0.0F,
        static_cast<float>(width), 0.0F, 1.0F, 1.0F};
    glDisable(GL_BLEND);
    glUseProgram(blurProgram);
    glUniform2f(blurViewportLocation, static_cast<float>(width),
                static_cast<float>(height));
    glUniform1i(blurTextureLocation, 0);
    glUniform2f(blurTexelSizeLocation, 1.0F / static_cast<float>(width),
                1.0F / static_cast<float>(height));
    glUniform1f(blurAmountLocation, std::clamp(amount, 0.0F, 1.0F));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurTexture);
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
  }

  void draw(int width, int height, std::span<const RectItem> rectangles,
            std::span<const TextItem> text,
            std::span<const ImageItem> images) const {
    glViewport(0, 0, width, height);
    glClearColor(0.10F, 0.12F, 0.16F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (width <= 0 || height <= 0) {
      return;
    }

    drawRectangles(width, height, rectangles);
    drawImages(width, height, images);
    glClearColor(0.10F, 0.12F, 0.16F, 1.0F);
    drawText(width, height, text);
  }

  void drawOverlay(int width, int height, std::span<const RectItem> rectangles,
                   std::span<const TextItem> text,
                   std::span<const ImageItem> images) const {
    if (width <= 0 || height <= 0) {
      return;
    }
    glViewport(0, 0, width, height);
    drawRectangles(width, height, rectangles);
    drawImages(width, height, images);
    drawText(width, height, text);
  }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() = default;

int Renderer::fontSize() const {
  return impl_->currentFontSize;
}

void Renderer::setFontSize(int size) {
  impl_->setFontSize(size);
}

std::string Renderer::fontFacePath() const {
  return impl_->currentFontPath;
}

std::string Renderer::fontFamily() const {
  return impl_->currentFontFamily;
}

std::string Renderer::fontStyle() const {
  return impl_->currentFontStyle;
}

std::vector<FontFace> Renderer::fontFaces() const {
  return impl_->fontFaces();
}

void Renderer::setFontFace(const std::string& path) {
  impl_->setFontFace(path);
}

void Renderer::useSystemFont() {
  const std::string path = Impl::systemFontPath();
  if (!path.empty()) {
    impl_->setFontFace(path);
  }
}

float Renderer::measureText(std::string_view text) const {
  return impl_->measureText(text);
}

void Renderer::draw(int width, int height, std::span<const RectItem> rectangles,
                    std::span<const TextItem> text,
                    std::span<const ImageItem> images) const {
  impl_->draw(width, height, rectangles, text, images);
}

void Renderer::drawClipped(int width, int height, const ClipRect& clip,
                           std::span<const RectItem> rectangles,
                           std::span<const TextItem> text,
                           std::span<const ImageItem> images) const {
  if (width <= 0 || height <= 0 || clip.width <= 0.0F ||
      clip.height <= 0.0F) {
    return;
  }

  const int left = std::clamp(static_cast<int>(std::floor(clip.x)), 0, width);
  const int right =
      std::clamp(static_cast<int>(std::ceil(clip.x + clip.width)), left, width);
  const int top = std::clamp(static_cast<int>(std::floor(clip.y)), 0, height);
  const int bottom =
      std::clamp(static_cast<int>(std::ceil(clip.y + clip.height)), top, height);
  if (left == right || top == bottom) {
    return;
  }

  glViewport(0, 0, width, height);
  glEnable(GL_SCISSOR_TEST);
  struct ScissorGuard {
    ~ScissorGuard() { glDisable(GL_SCISSOR_TEST); }
  };
  const ScissorGuard scissorGuard;
  glScissor(left, height - bottom, right - left, bottom - top);
  impl_->drawOverlay(width, height, rectangles, text, images);
}

void Renderer::blurCurrentFramebuffer(int width, int height, float amount) const {
  impl_->blurCurrentFramebuffer(width, height, amount);
}

void Renderer::drawOverlay(int width, int height,
                           std::span<const RectItem> rectangles,
                           std::span<const TextItem> text,
                           std::span<const ImageItem> images) const {
  impl_->drawOverlay(width, height, rectangles, text, images);
}

}  // namespace render
