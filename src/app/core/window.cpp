#include "core/window.hpp"

#include <stdexcept>

namespace core {

Window::Window(int width, int height, const char* title) {
  if (glfwInit() != GLFW_TRUE) {
    throw std::runtime_error("Failed to initialize GLFW");
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
  if (window_ == nullptr) {
    glfwTerminate();
    throw std::runtime_error("Failed to create an OpenGL 4.6 window");
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);
}

Window::~Window() {
  if (window_ != nullptr) {
    glfwDestroyWindow(window_);
  }
  glfwTerminate();
}

bool Window::shouldClose() const {
  return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void Window::pollEvents() const {
  glfwPollEvents();
}

void Window::swapBuffers() const {
  glfwSwapBuffers(window_);
}

void Window::getFramebufferSize(int& width, int& height) const {
  glfwGetFramebufferSize(window_, &width, &height);
}

GLFWwindow* Window::nativeHandle() const {
  return window_;
}

}  // namespace core
