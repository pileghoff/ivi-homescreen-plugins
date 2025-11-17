/*
 * Copyright 2024 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <GLES2/gl2.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_homescreen.h>
#include <glib.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <chrono>

#include <atomic>
#include <thread>

class ClairNativeVideoPlugin : public flutter::Plugin {
 public:
  using Clock = std::chrono::steady_clock;
  explicit ClairNativeVideoPlugin(flutter::PluginRegistrarDesktop* registrar);
  ~ClairNativeVideoPlugin() override;

  // Disallow copy and move.
  ClairNativeVideoPlugin(const ClairNativeVideoPlugin&) = delete;
  ClairNativeVideoPlugin& operator=(const ClairNativeVideoPlugin&) = delete;

  GstElement* pipeline_ = nullptr;
  void InitializeGStreamer();

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void OnFrameAvailable(GstBuffer* buffer);

  static GstFlowReturn OnNewSample(GstAppSink* sink, gpointer data);
  static gboolean BusCallback(GstBus* bus, GstMessage* msg, gpointer user_data);

  flutter::PluginRegistrarDesktop* registrar_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::unique_ptr<flutter::GpuSurfaceTexture> gpu_surface_texture_;
  FlutterDesktopGpuSurfaceDescriptor surface_descriptor_{};
  GLuint texture_id_{};

  std::thread gst_thread_;
  GMainLoop* main_loop_ = nullptr;
  GstElement* src_ = nullptr;
  Clock::time_point debounce_;
};
