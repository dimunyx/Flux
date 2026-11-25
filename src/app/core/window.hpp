#pragma once

#include <GLFW/glfw3.h>

namespace core {

class Window {
 public:
  Window(int width, int height, const char* title);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  bool shouldClose() const;
  void pollEvents() const;
  void swapBuffers() const;
  void getFramebufferSize(int& width, int& height) const;
  GLFWwindow* nativeHandle() const;

 private:
  GLFWwindow* window_ = nullptr;
};

}  // namespace core
