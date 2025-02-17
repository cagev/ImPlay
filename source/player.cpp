// Copyright (c) 2022 tsl0922. All rights reserved.
// SPDX-License-Identifier: GPL-2.0-only

#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include "player.h"
#include "helpers.h"

namespace ImPlay {
Player::Player(Config* config, Dispatch* dispatch, GLFWwindow* window, Mpv* mpv, const char* title) : Views::View() {
  this->config = config;
  this->dispatch = dispatch;
  this->window = window;
  this->mpv = mpv;
  this->title = title;

  cmd = new Command(config, window, mpv);
}

Player::~Player() { delete cmd; }

bool Player::init(OptionParser& parser) {
  logoTexture = ImGui::LoadTexture("icon.png");

  mpv->option("config", "yes");
  mpv->option("osc", "yes");
  mpv->option("input-default-bindings", "yes");
  mpv->option("input-vo-keyboard", "yes");
  mpv->option("osd-playing-msg", "${media-title}");
  mpv->option("screenshot-directory", "~~desktop/");
  if (!config->Data.Mpv.UseConfig) mpv->option("config-dir", datadir().c_str());

  for (const auto& [key, value] : parser.options) {
    if (int err = mpv->option(key.c_str(), value.c_str()); err < 0) {
      fmt::print(fg(fmt::color::red), "mpv: {} [{}={}]\n", mpv_error_string(err), key, value);
      return false;
    }
  }

  cmd->init();
  mpv->init();
  initMpv();

  for (auto& path : parser.paths) mpv->commandv("loadfile", path.c_str(), "append-play", nullptr);

  mpv->command("keybind MBTN_RIGHT ignore");

  return true;
}

void Player::draw() {
  drawLogo();
  cmd->draw();
}

void Player::drawLogo() {
  if (logoTexture == nullptr || mpv->forceWindow() || fileOpen) return;

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing;
  const float imageWidth = std::min(viewport->WorkSize.x, viewport->WorkSize.y) * 0.2f;
  const ImVec2 imageSize(imageWidth, imageWidth);
  ImGui::SetNextWindowSize(imageSize * 1.5f, ImGuiCond_Always);
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_WindowShadow, ImVec4(0, 0, 0, 0));
  ImGui::Begin("Logo", nullptr, flags);
  ImGui::SetCursorPos((ImGui::GetWindowSize() - imageSize) * 0.5f);
  ImGui::Image(logoTexture, imageSize);
  ImGui::End();
  ImGui::PopStyleColor();
}

void Player::render(int w, int h) {
  glfwMakeContextCurrent(window);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (hasFile() || mpv->forceWindow()) mpv->render(w, h);

  bool renderGui = renderGui_;

  if (renderGui) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  }

  glfwSwapBuffers(window);
  glfwMakeContextCurrent(nullptr);

  if (renderGui && ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    dispatch->sync(
        [](void* data) {
          ImGui::UpdatePlatformWindows();
          ImGui::RenderPlatformWindowsDefault();
        },
        nullptr);
  }
}

void Player::shutdown() { mpv->command(config->Data.Mpv.WatchLater ? "quit-watch-later" : "quit"); }

void Player::onCursorEvent(double x, double y) {
  std::string xs = std::to_string((int)x);
  std::string ys = std::to_string((int)y);
  mpv->commandv("mouse", xs.c_str(), ys.c_str(), nullptr);
}

void Player::onMouseEvent(int button, int action, int mods) {
  auto s = actionMappings.find(action);
  if (s == actionMappings.end()) return;
  const std::string cmd = s->second;

  std::vector<std::string> keys;
  translateMod(keys, mods);
  s = mbtnMappings.find(button);
  if (s == actionMappings.end()) return;
  keys.push_back(s->second);
  const std::string arg = format("{}", join(keys, "+"));
  mpv->commandv(cmd.c_str(), arg.c_str(), nullptr);
}

void Player::onScrollEvent(double x, double y) {
  if (abs(x) > 0) {
    mpv->command(x > 0 ? "keypress WHEEL_LEFT" : "keypress WHEEL_RIGH");
    mpv->command(x > 0 ? "keyup WHEEL_LEFT" : "keyup WHEEL_RIGH");
  }
  if (abs(y) > 0) {
    mpv->command(y > 0 ? "keypress WHEEL_UP" : "keypress WHEEL_DOWN");
    mpv->command(y > 0 ? "keyup WHEEL_UP" : "keyup WHEEL_DOWN");
  }
}

void Player::onKeyEvent(int key, int scancode, int action, int mods) {
  auto s = actionMappings.find(action);
  if (s == actionMappings.end()) return;
  const std::string cmd = s->second;

  std::string name;
  if (mods & GLFW_MOD_SHIFT) {
    s = shiftMappings.find(key);
    if (s != shiftMappings.end()) {
      name = s->second;
      mods &= ~GLFW_MOD_SHIFT;
    }
  }
  if (name.empty()) {
    s = keyMappings.find(key);
    if (s == keyMappings.end()) return;
    name = s->second;
  }

  std::vector<std::string> keys;
  translateMod(keys, mods);
  keys.push_back(name);
  const std::string arg = format("{}", join(keys, "+"));
  mpv->commandv(cmd.c_str(), arg.c_str(), nullptr);
}

void Player::onDropEvent(int count, const char** paths) {
  std::sort(paths, paths + count, [](const auto& a, const auto& b) { return strcmp(a, b) < 0; });
  for (int i = 0; i < count; i++) {
    mpv->commandv("loadfile", paths[i], i > 0 ? "append-play" : "replace", nullptr);
  }
}

void Player::initMpv() {
  mpv->observeEvent(MPV_EVENT_SHUTDOWN, [this](void* data) { glfwSetWindowShouldClose(window, GLFW_TRUE); });

  mpv->observeEvent(MPV_EVENT_VIDEO_RECONFIG, [this](void* data) {
    int width = (int)mpv->property<int64_t, MPV_FORMAT_INT64>("dwidth");
    int height = (int)mpv->property<int64_t, MPV_FORMAT_INT64>("dheight");
    if (width > 0 && height > 0) {
      glfwSetWindowSize(window, width, height);
      if (mpv->property<int, MPV_FORMAT_FLAG>("keepaspect-window")) glfwSetWindowAspectRatio(window, width, height);
    }
  });

  mpv->observeEvent(MPV_EVENT_START_FILE, [this](void* data) { fileOpen = true; });

  mpv->observeEvent(MPV_EVENT_END_FILE, [this](void* data) {
    fileOpen = false;
    glfwSetWindowTitle(window, title);
    glfwSetWindowAspectRatio(window, GLFW_DONT_CARE, GLFW_DONT_CARE);
  });

  mpv->observeEvent(MPV_EVENT_CLIENT_MESSAGE, [this](void* data) {
    ImGuiIO& io = ImGui::GetIO();
    renderGui_ = false;
    io.SetAppAcceptingEvents(false);

    auto msg = static_cast<mpv_event_client_message*>(data);
    cmd->execute(msg->num_args, msg->args);

    renderGui_ = true;
    io.SetAppAcceptingEvents(true);
  });

  mpv->observeProperty("media-title", MPV_FORMAT_STRING, [this](void* data) {
    char* title = static_cast<char*>(*(char**)data);
    glfwSetWindowTitle(window, title);
  });

  mpv->observeProperty("border", MPV_FORMAT_FLAG, [this](void* data) {
    bool enable = static_cast<bool>(*(int*)data);
    glfwSetWindowAttrib(window, GLFW_DECORATED, enable ? GLFW_TRUE : GLFW_FALSE);
  });

  mpv->observeProperty("window-maximized", MPV_FORMAT_FLAG, [this](void* data) {
    bool enable = static_cast<bool>(*(int*)data);
    if (enable)
      glfwMaximizeWindow(window);
    else
      glfwRestoreWindow(window);
  });

  mpv->observeProperty("window-minimized", MPV_FORMAT_FLAG, [this](void* data) {
    bool enable = static_cast<bool>(*(int*)data);
    if (enable)
      glfwIconifyWindow(window);
    else
      glfwRestoreWindow(window);
  });

  mpv->observeProperty("window-scale", MPV_FORMAT_DOUBLE, [this](void* data) {
    double scale = static_cast<double>(*(double*)data);
    int w = (int)mpv->property<int64_t, MPV_FORMAT_INT64>("dwidth");
    int h = (int)mpv->property<int64_t, MPV_FORMAT_INT64>("dheight");
    if (w > 0 && h > 0) glfwSetWindowSize(window, (int)(w * scale), (int)(h * scale));
  });

  mpv->observeProperty("fullscreen", MPV_FORMAT_FLAG, [this](void* data) {
    bool enable = static_cast<bool>(*(int*)data);
    bool isFullscreen = glfwGetWindowMonitor(window) != nullptr;
    if (isFullscreen == enable) return;

    static int x, y, w, h;
    if (enable) {
      glfwGetWindowPos(window, &x, &y);
      glfwGetWindowSize(window, &w, &h);
      GLFWmonitor* monitor = glfwGetPrimaryMonitor();
      const GLFWvidmode* mode = glfwGetVideoMode(monitor);
      glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else
      glfwSetWindowMonitor(window, nullptr, x, y, w, h, 0);
  });

  mpv->observeProperty("ontop", MPV_FORMAT_FLAG, [this](void* data) {
    bool enable = static_cast<bool>(*(int*)data);
    glfwSetWindowAttrib(window, GLFW_FLOATING, enable ? GLFW_TRUE : GLFW_FALSE);
  });
}
}  // namespace ImPlay