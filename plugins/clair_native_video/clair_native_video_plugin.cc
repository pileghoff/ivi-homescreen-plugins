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

#include "clair_native_video_plugin.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <chrono>
#include <thread>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_homescreen.h>
#include <flutter/standard_method_codec.h>

#include "plugins/common/logging.h"

#include <glib.h>  // for g_idle_add, GSource functions

constexpr int kTextureWidth = 892;
constexpr int kTextureHeight = 600;

using Clock = std::chrono::steady_clock;

ClairNativeVideoPlugin::ClairNativeVideoPlugin(
    flutter::PluginRegistrarDesktop* registrar)
    : registrar_(registrar), debounce_(Clock::now()) {
  gst_init(nullptr, nullptr);
  SPDLOG_DEBUG("ClairNativeVideoPlugin constructor");
  channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "clair_native_video",
      &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    HandleMethodCall(call, std::move(result));
  });
}

ClairNativeVideoPlugin::~ClairNativeVideoPlugin() {
  SPDLOG_DEBUG("ClairNativeVideoPlugin destructor");
  if (main_loop_) {
    g_main_loop_quit(main_loop_);
  }
  if (gst_thread_.joinable()) {
    gst_thread_.join();
  }
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  if (main_loop_) {
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
  }

  if (texture_id_) {
    registrar_->texture_registrar()->UnregisterTexture(texture_id_);
    texture_id_ = 0;
  }
}

void ClairNativeVideoPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  SPDLOG_DEBUG("HandleMethodCall: {}", method_call.method_name());
  if (method_call.method_name().compare("create") == 0) {
    InitializeGStreamer();
    if (texture_id_) {
      result->Success(
          flutter::EncodableValue(static_cast<int64_t>(texture_id_)));
    } else {
      result->Error("texture_creation_failed", "Failed to create texture.");
    }
  } else {
    result->NotImplemented();
  }
}

GstFlowReturn ClairNativeVideoPlugin::OnNewSample(GstAppSink* sink,
                                                  gpointer data) {
  auto* self = static_cast<ClairNativeVideoPlugin*>(data);
  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) {
    return GST_FLOW_ERROR;
  }

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  if (buffer) {
    self->OnFrameAvailable(buffer);
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// Forward declaration for idle callback below
static gboolean RecreatePipelineIdle(gpointer data);

gboolean ClairNativeVideoPlugin::BusCallback(GstBus* bus,
                                             GstMessage* msg,
                                             gpointer data) {
  auto* self = static_cast<ClairNativeVideoPlugin*>(data);
  auto now = Clock::now();
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      SPDLOG_ERROR("GStreamer ERROR: {} ({})", err ? err->message : "unknown",
                   dbg ? dbg : "no-debug");
      if (err)
        g_error_free(err);
      if (dbg)
        g_free(dbg);
      // fall through to teardown/reconnect logic below
    }
    case GST_MESSAGE_EOS: {
      // Debounce rapid repeated errors
      if ((now - self->debounce_) <= std::chrono::milliseconds(1000)) {
        return TRUE;
      }
      self->debounce_ = now;
      g_print(
          "Producer disappeared. Recreating pipeline and reconnecting...\n");

      if (self->pipeline_) {
        // Stop and free the pipeline so elements (including nvunixfdsrc) are
        // recreated.
        gst_element_set_state(self->pipeline_, GST_STATE_NULL);

        // remove bus watch? It's tied to the pipeline - unref will remove it
        gst_object_unref(self->pipeline_);
        self->pipeline_ = nullptr;
      }

      // Schedule pipeline recreation on the main loop thread via idle callback.
      // If main_loop_ isn't created yet (shouldn't happen), call Initialize
      // directly.
      if (self->main_loop_) {
        // g_idle_add will run on the main loop thread that is running
        // g_main_loop_run.
        g_idle_add(RecreatePipelineIdle, self);
      } else {
        // fall back: initialize synchronously
        self->InitializeGStreamer();
      }

      return TRUE;
    }

    default:
      return TRUE;
  }
}

static gboolean RecreatePipelineIdle(gpointer data) {
  ClairNativeVideoPlugin* self = static_cast<ClairNativeVideoPlugin*>(data);
  // Defensive: only re-create if pipeline isn't already present
  if (!self->pipeline_) {
    SPDLOG_DEBUG("RecreatePipelineIdle: calling InitializeGStreamer()");
    self->InitializeGStreamer();
  } else {
    SPDLOG_DEBUG("RecreatePipelineIdle: pipeline already exists, skipping");
  }
  return G_SOURCE_REMOVE;  // run once
}

void ClairNativeVideoPlugin::OnFrameAvailable(GstBuffer* buffer) {
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    registrar_->texture_registrar()->TextureMakeCurrent();
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTextureWidth, kTextureHeight,
                    GL_RGBA, GL_UNSIGNED_BYTE, map.data);
    registrar_->texture_registrar()->MarkTextureFrameAvailable(texture_id_);
    registrar_->texture_registrar()->TextureClearCurrent();
    gst_buffer_unmap(buffer, &map);
  } else {
    SPDLOG_ERROR("Failed to map GStreamer buffer for reading.");
  }
}

void ClairNativeVideoPlugin::InitializeGStreamer() {
  SPDLOG_DEBUG("InitializeGStreamer");
  if (pipeline_) {
    SPDLOG_DEBUG("GStreamer pipeline already initialized");
    return;
  }

  // Ensure main loop exists before we create pipeline so idle callbacks have a
  // loop.
  if (!main_loop_) {
    main_loop_ = g_main_loop_new(nullptr, FALSE);
    gst_thread_ = std::thread([this]() { g_main_loop_run(main_loop_); });
    // slight delay to ensure main loop has started before pipeline creation
    // (optional)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // 1. Create texture (only once; if you need to recreate texture on pipeline
  // restart change logic)
  if (!texture_id_) {
    registrar_->texture_registrar()->TextureMakeCurrent();
    glGenTextures(1, &texture_id_);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kTextureWidth, kTextureHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    registrar_->texture_registrar()->TextureClearCurrent();

    surface_descriptor_ = {
        .struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor),
        .handle = &texture_id_,
        .width = kTextureWidth,
        .height = kTextureHeight,
        .visible_width = kTextureWidth,
        .visible_height = kTextureHeight,
        .format = kFlutterDesktopPixelFormatRGBA8888,
        .release_callback = [](void* /* release_context */) {},
        .release_context = this,
    };

    gpu_surface_texture_ = std::make_unique<flutter::GpuSurfaceTexture>(
        kFlutterDesktopGpuSurfaceTypeGlTexture2D,
        [this](size_t, size_t) { return &surface_descriptor_; });

    flutter::TextureVariant texture(*gpu_surface_texture_);
    registrar_->texture_registrar()->RegisterTexture(&texture);
    SPDLOG_DEBUG("Texture created with id: {}",
                 static_cast<int64_t>(texture_id_));
  }

  // 2. Create GStreamer pipeline elements
  pipeline_ = gst_pipeline_new("consumer");
  src_ = gst_element_factory_make("nvunixfdsrc", "src");
  GstElement* conv = gst_element_factory_make("nvvidconv", "converter");
  GstElement* capsfilter = gst_element_factory_make("capsfilter", nullptr);
  GstElement* appsink = gst_element_factory_make("appsink", "glsink");

  if (!pipeline_ || !src_ || !conv || !capsfilter || !appsink) {
    SPDLOG_ERROR("Failed to create GStreamer elements");
    if (pipeline_) {
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    if (src_) {
      gst_object_unref(src_);
      src_ = nullptr;
    }
    if (conv)
      gst_object_unref(conv);
    if (capsfilter)
      gst_object_unref(capsfilter);
    if (appsink)
      gst_object_unref(appsink);
    return;
  }

  // Set properties
  g_object_set(src_, "socket-path", "/tmp/video.sock", "connection-attempts",
               -1, "connection-interval", guint64(500000), NULL);

  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, "RGBA", "width", G_TYPE_INT,
      kTextureWidth, "height", G_TYPE_INT, kTextureHeight, NULL);
  g_object_set(capsfilter, "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, "drop", TRUE,
               "max-buffers", 1, NULL);

  // Add elements to bin
  gst_bin_add_many(GST_BIN(pipeline_), src_, conv, capsfilter, appsink, NULL);

  // Link elements
  if (!gst_element_link(src_, conv) || !gst_element_link(conv, capsfilter) ||
      !gst_element_link(capsfilter, appsink)) {
    SPDLOG_ERROR("GStreamer elements linking failed!");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return;
  }

  // Connect bus callback
  GstBus* bus = gst_element_get_bus(pipeline_);
  gst_bus_add_watch(bus, BusCallback, this);
  gst_object_unref(bus);

  // Connect appsink callback
  g_signal_connect(appsink, "new-sample", G_CALLBACK(OnNewSample), this);

  // Start playing
  GstStateChangeReturn ret =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    SPDLOG_ERROR(
        "Failed to set pipeline to PLAYING (state change failure). Will "
        "teardown and try again.");
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    // schedule a retry on main loop
    if (main_loop_)
      g_idle_add(RecreatePipelineIdle, this);
    return;
  }

  SPDLOG_DEBUG("Pipeline started (PLAYING).");
}
