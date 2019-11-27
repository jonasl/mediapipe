// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// An example of sending OpenCV webcam frames into a MediaPipe graph.
// This example requires a linux computer and a GPU with EGL support drivers.

#include <gst/gst.h>
#include <gst/gl/gstglcontext.h>
#include <gst/gl/gstgldisplay.h>
#include <gst/gl/gstglmemory.h>

#include "absl/strings/str_format.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/calculator_graph.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"
#include "mediapipe/framework/port/commandlineflags.h"
#include "mediapipe/framework/port/file_helpers.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/gpu/gl_calculator_helper.h"
#include "mediapipe/gpu/gpu_buffer.h"
#include "mediapipe/gpu/gpu_buffer_format.h"
#include "mediapipe/gpu/gpu_shared_data_internal.h"

constexpr char kInputStream[] = "input_video";
constexpr char kOutputStream[] = "output_video";
constexpr char kWindowName[] = "MediaPipe";

DEFINE_string(
    calculator_graph_config_file, "",
    "Name of file containing text format CalculatorGraphConfig proto.");
DEFINE_int32(input_video_width, 640, "Input video width");
DEFINE_int32(input_video_height, 480, "Input video height");

class GstWrapper
{
public:
  GstWrapper() {
    gst_init(nullptr, nullptr);

    // TODO: This needs to be configurable, support MJPEG/h264 cameras,
    // v4l2 device selection, video decoding etc.
    // For now use gst_parse_launch for convenience.
    std::string spec = absl::StrFormat("v4l2src ! video/x-raw,width=%d,height=%d ! "
      " glupload ! glcolorconvert ! glvideoflip video-direction=horiz name=flip ! "
      " glimagesink name=glsink",
      FLAGS_input_video_width, FLAGS_input_video_height);
    LOG(INFO) << "Parsing: " << spec;

    pipeline_ = gst_parse_launch(spec.c_str(), /* TODO: pass in and parse GError */ nullptr);

    GstElement *element;
    CHECK(element = GST_ELEMENT(gst_bin_get_by_name(GST_BIN(pipeline_), "glsink")));

    // Disabling sync on the sink is required when our processing latency is high,
    // or the sink will just drop all frames it considers late.
    // TODO: For non live (i.e. camera) sources consider dropping frames instead
    // of delaying them.
    g_object_set(element, "sync", FALSE, nullptr);
    gst_object_unref(element);

    CHECK(element = GST_ELEMENT(gst_bin_get_by_name(GST_BIN(pipeline_), "flip")));
    // Set up a pad probe to where we swap out the camera frames for mediapipe
    // output frames.
    GstPad *pad = gst_element_get_static_pad(element, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
        reinterpret_cast<GstPadProbeCallback>(GstWrapper::OnPadProbeBuffer),
        this, nullptr);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
        reinterpret_cast<GstPadProbeCallback>(GstWrapper::OnPadProbeQuery),
        this, nullptr);

    gst_object_unref(pad);
    gst_object_unref(element);

    // TODO: sync bus handler to catch errors.
  }

  ::mediapipe::Status Start() {
    std::string calculator_graph_config_contents;
    MP_RETURN_IF_ERROR(mediapipe::file::GetContents(
        FLAGS_calculator_graph_config_file, &calculator_graph_config_contents));
    mediapipe::CalculatorGraphConfig config =
      mediapipe::ParseTextProtoOrDie<mediapipe::CalculatorGraphConfig>(
          calculator_graph_config_contents);
    LOG(INFO) << "Initialize the calculator graph.";
    MP_RETURN_IF_ERROR(graph_.Initialize(config));

    // TODO: Proper state change error handling.

    // GL contexts are created in the READY state.
    LOG(INFO) << "Setting GStreamer to READY";
    gst_element_set_state(pipeline_, GST_STATE_READY);
    CHECK_NE(gst_element_get_state (pipeline_, nullptr, nullptr, GST_CLOCK_TIME_NONE),
        GST_STATE_CHANGE_FAILURE);

    GstElement *element;
    CHECK(element = GST_ELEMENT(gst_bin_get_by_name(GST_BIN(pipeline_), "glsink")));
    g_object_get(G_OBJECT(element), "context", &gst_gl_context_, nullptr);
    gst_object_unref(element);
    CHECK(gst_gl_context_) << "Couldn't get GstGLContext";
    CHECK_EQ(gst_gl_context_get_gl_platform(gst_gl_context_), GST_GL_PLATFORM_EGL);

    // Configure context sharing with mediapipe.
    LOG(INFO) << "Initialize the GPU.";
    GstGLDisplay *gst_display = gst_gl_context_get_display(gst_gl_context_);
    ASSIGN_OR_RETURN(auto gpu_resources, mediapipe::GpuResources::Create(
        reinterpret_cast<mediapipe::PlatformGlContext>(gst_gl_context_get_gl_context(gst_gl_context_)),
        reinterpret_cast<mediapipe::PlatformDisplay>(gst_gl_display_get_handle(gst_display))));
    MP_RETURN_IF_ERROR(graph_.SetGpuResources(std::move(gpu_resources)));
    gpu_helper_.InitializeForTest(graph_.GetGpuResources().get());

    gst_object_unref(gst_display);

    LOG(INFO) << "Start running the calculator graph.";
    status_or_poller_ = graph_.AddOutputStreamPoller(kOutputStream);
    CHECK(status_or_poller_.ok());
    MP_RETURN_IF_ERROR(graph_.StartRun({}));

    LOG(INFO) << "Setting GStreamer to PLAYING";
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);

    // Wait until up and running or failed.
    CHECK_NE(gst_element_get_state (pipeline_, nullptr, nullptr, GST_CLOCK_TIME_NONE),
          GST_STATE_CHANGE_FAILURE);
    LOG(INFO) << "GStreamer now in PLAYING state";
    return ::mediapipe::OkStatus();
  }

  void Stop() {
    LOG(INFO) << "Setting GStreamer to NULL";
    gst_element_set_state(pipeline_, GST_STATE_NULL);
  }

  ~GstWrapper() {
    Stop();
    gst_object_unref(gst_gl_context_);
    gst_object_unref(pipeline_);
  }

  static GstPadProbeReturn OnPadProbeBuffer(GstPad *pad, GstPadProbeInfo *info, GstWrapper *self) {
    return self->HandlePadProbeBuffer(info);
  }

  static GstPadProbeReturn OnPadProbeQuery(GstPad *pad, GstPadProbeInfo *info, GstWrapper *self) {
    return self->HandlePadProbeQuery(info);
  }

  static void OnPacketDestroy(mediapipe::Packet *packet) {
    delete packet;
  }

private:
  // Called in GStreamer streaming thread context.
  GstPadProbeReturn HandlePadProbeBuffer(GstPadProbeInfo *info) {
    CHECK(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER);
    GstBuffer *in_buf = gst_pad_probe_info_get_buffer(info);

    // Push input texture into mediapipe graph.
    GstMemory *memory = gst_buffer_peek_memory(in_buf, 0);
    GstVideoMeta *meta = gst_buffer_get_video_meta(in_buf);
    CHECK_EQ(meta->n_planes, 1);  // Only one plane for RGB.
    CHECK(gst_is_gl_memory(memory));
    guint input_texture_id = gst_gl_memory_get_texture_id(GST_GL_MEMORY_CAST(memory));

    // TODO: Wait on sync object in MP GL context.
    auto input_packet = mediapipe::MakePacket<mediapipe::GpuBuffer>(
      mediapipe::GlTextureBuffer::Wrap(GL_TEXTURE_2D, input_texture_id,
          meta->width, meta->height,
          mediapipe::GpuBufferFormat::kBGRA32,
          nullptr));
    graph_.AddPacketToInputStream(kInputStream,
        input_packet.At(mediapipe::Timestamp(frame_timestamp_++)));

    // Get the mediapipe output packet. Since we're in GStreamer streaming
    // thread context any long blocking operation here slows down the pipeline.
    // We're essentially acting as a GStreamer filter.
    mediapipe::OutputStreamPoller& poller = status_or_poller_.ValueOrDie();
    mediapipe::Packet *out_packet = new mediapipe::Packet();
    CHECK(poller.Next(out_packet));

    GLuint out_tex_id;
    int out_width;
    int out_height;
    gpu_helper_.RunInGlContext(
      [&out_packet, &out_tex_id, &out_width, &out_height, this]() -> ::mediapipe::Status {
        auto& gpu_frame = out_packet->Get<mediapipe::GpuBuffer>();
        auto texture = gpu_helper_.CreateSourceTexture(gpu_frame);
        out_tex_id = texture.name();
        out_width = texture.width();
        out_height = texture.height();
        return ::mediapipe::OkStatus();
      });

    double since_last_ms = absl::ToDoubleMilliseconds(absl::Now() - last_frame_);
    last_frame_ = absl::Now();
    LOG(INFO) << absl::StrFormat("tex %u %dx%d %.2f ms (%.2f fps)",
        out_tex_id, out_width, out_height,
        since_last_ms, 1000 / since_last_ms);

    // TODO: Wait on sync object in GST GL context.

    // We now have the output texture, wrap it in a GstBuffer and replace input.
    GstVideoInfo vinfo;
    gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, meta->width, meta->height);
    GstGLAllocationParams *params = reinterpret_cast<GstGLAllocationParams*>(
        gst_gl_video_allocation_params_new_wrapped_texture(
          gst_gl_context_,
          NULL /* alloc_params */,
          &vinfo,
          0 /* plane */,
          NULL /* valign */,
          GST_GL_TEXTURE_TARGET_2D,
          GST_GL_RGBA,
          out_tex_id,
          out_packet,
          reinterpret_cast<GDestroyNotify>(GstWrapper::OnPacketDestroy)));

    GstAllocator *gl_allocator = gst_allocator_find(GST_GL_MEMORY_ALLOCATOR_NAME);
    CHECK(gl_allocator);
    GstGLMemory *gl_memory = GST_GL_MEMORY_CAST(gst_gl_base_memory_alloc(
        GST_GL_BASE_MEMORY_ALLOCATOR(gl_allocator), params));
    CHECK(gl_memory);
    CHECK_EQ(gst_gl_memory_get_texture_id(gl_memory), out_tex_id);
    gst_gl_allocation_params_free(params);
    gst_object_unref(gl_allocator);
    

    GstBuffer *out_buf = gst_buffer_new();
    CHECK(gst_buffer_copy_into(out_buf, in_buf,
        static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS),
        0 /* offset */,
        0 /* size, not used since not copying data */));
    gst_buffer_add_video_meta(out_buf, meta->flags, meta->format, meta->width, meta->height);
    gst_buffer_append_memory(out_buf, GST_MEMORY_CAST(gl_memory));

    // Replace the original buffer with ours.
    info->data = out_buf;
    gst_buffer_unref(in_buf);

    return GST_PAD_PROBE_OK;
  }

  // Called in GStreamer streaming thread context.
  GstPadProbeReturn HandlePadProbeQuery(GstPadProbeInfo *info) {
    CHECK(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM);
    GstQuery *query = gst_pad_probe_info_get_query(info);

    // Clobber declared sink support for affine transformations so glvideoflip
    // gives us flipped buffer in the buffer probe.
    if (GST_QUERY_TYPE(query) == GST_QUERY_ALLOCATION) {
      LOG(INFO) << "Forwarding allocation query";
      GstElement *element;
      GstPad *pad;
      CHECK(element = GST_ELEMENT(gst_bin_get_by_name(GST_BIN(pipeline_), "glsink")));
      CHECK(pad = gst_element_get_static_pad(element, "sink"));
      if (gst_pad_query(pad, query)) {
        LOG(INFO) << "Sink query succeeded";
        guint index;
        if (gst_query_find_allocation_meta(query, GST_VIDEO_AFFINE_TRANSFORMATION_META_API_TYPE, &index)) {
          LOG(INFO) << "Remove meta API at index " << index;
          gst_query_remove_nth_allocation_meta(query, index);
        }
      }
      gst_object_unref(pad);
      gst_object_unref(element);
      return GST_PAD_PROBE_HANDLED;
    }

    return GST_PAD_PROBE_OK;
  }

  mediapipe::CalculatorGraph graph_;
  mediapipe::StatusOrPoller status_or_poller_;
  mediapipe::GlCalculatorHelper gpu_helper_;
  GstElement *pipeline_ = nullptr;
  GstGLContext *gst_gl_context_ = nullptr;
  size_t frame_timestamp_ = 0;  // TODO: Use real timestamp
  absl::Mutex mutex_;
  absl::Time last_frame_ = absl::Now();
};


::mediapipe::Status RunMPPGraph() {
  GstWrapper gst;
  MP_RETURN_IF_ERROR(gst.Start());

  std::string line;
  std::getline(std::cin, line);
  gst.Stop();
  return ::mediapipe::OkStatus();
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  ::mediapipe::Status run_status = RunMPPGraph();
  if (!run_status.ok()) {
    LOG(ERROR) << "Failed to run the graph: " << run_status.message();
  } else {
    LOG(INFO) << "Success!";
  }
  return 0;
}
