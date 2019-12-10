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

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include "absl/strings/str_format.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"
#include "mediapipe/framework/port/commandlineflags.h"
#include "mediapipe/framework/port/file_helpers.h"
#include "mediapipe/framework/port/opencv_highgui_inc.h"
#include "mediapipe/framework/port/opencv_imgproc_inc.h"
#include "mediapipe/framework/port/opencv_video_inc.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/status.h"

constexpr char kInputStream[] = "input_video";
constexpr char kOutputStream[] = "output_video";
constexpr char kWindowName[] = "MediaPipe";
constexpr uint32_t kCameraFrameTimeoutMs = 1000;

DEFINE_string(
    calculator_graph_config_file, "",
    "Name of file containing text format CalculatorGraphConfig proto.");
DEFINE_string(input_video_path, "",
              "Full path of video to load. "
              "If not provided, attempt to use a webcam.");
DEFINE_string(output_video_path, "",
              "Full path of where to save result (.mp4 only). "
              "If not provided, show result in a window.");
DEFINE_int32(input_video_width, 640, "Input video width");
DEFINE_int32(input_video_height, 480, "Input video height");

class SimpleGstCamera
{
public:

  SimpleGstCamera() {
    gst_init (nullptr, nullptr);

    // TODO: This needs to be configurable, support MJPEG/h264 cameras,
    // v4l2 device selection etc. For now use gst_parse_launch for convenience.
    std::string spec = absl::StrFormat("v4l2src ! video/x-raw,width=%d,height=%d ! "
      " glfilterbin filter=\"glvideoflip video-direction=horiz ! glcolorscale\" !"
      " video/x-raw,format=RGB ! appsink name=appsink",
      FLAGS_input_video_width, FLAGS_input_video_height);
    LOG(INFO) << "Parsing: " << spec;

    pipeline_ = gst_parse_launch(spec.c_str(), /* TODO: pass in and parse GError */ NULL);
    appsink_ = GST_APP_SINK(gst_bin_get_by_name (GST_BIN(pipeline_), "appsink"));
    g_object_set(G_OBJECT(appsink_), "emit-signals", TRUE, "sync", FALSE, nullptr);
    g_signal_connect(appsink_, "new-sample",
        G_CALLBACK (SimpleGstCamera::OnNewSample), this);
    // TODO: sync bus handler to catch errors.
  }

  void Start() {
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);

    // Wait until  up and running or failed.
    // TODO: Proper error handling.
    CHECK_NE(gst_element_get_state (pipeline_, NULL, NULL, GST_CLOCK_TIME_NONE),
          GST_STATE_CHANGE_FAILURE);
  }

  void Stop() {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
  }

  double frame_rate() {
    if (fps_ == 0.0) {
      GetFrame();
    }
    return fps_;
  }

  std::unique_ptr<mediapipe::ImageFrame> GetFrame(uint32_t timeout_ms=kCameraFrameTimeoutMs) {
    GstBuffer *buffer = nullptr;
    {
      absl::MutexLock lock(&mutex_);
      mutex_.AwaitWithTimeout(
          absl::Condition(this, &SimpleGstCamera::HaveBuffer),
          absl::Milliseconds(timeout_ms));
      buffer = buffer_;
      buffer_ = nullptr;
    }

    if (buffer) {
      // Wrap the buffer in an ImageFrame.
      // TODO: Also return timestamp.
      double since_last_ms = absl::ToDoubleMilliseconds(absl::Now() - last_frame_);
      last_frame_ = absl::Now();

      GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
      CHECK_EQ(meta->n_planes, 1);  // Only one plane for RGB.
      LOG(INFO) << absl::StrFormat("Frame: %ux%u, stride %d, ts %" GST_TIME_FORMAT
        " since_last %.2f ms (%.2f fps)",
        meta->width, meta->height, meta->stride[0],
        GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buffer)),
        since_last_ms, 1000 / since_last_ms);

      // Map buffer for reading.
      // TODO: Is writing to the buffer important here?
      GstMapInfo map;
      CHECK(gst_buffer_map(buffer, &map, GST_MAP_READ));

      // Deleter for unmapping and unreffing.
      auto deleter = [buffer, map](uint8*) {
        gst_buffer_unmap(buffer, const_cast<GstMapInfo*>(&map));
        gst_buffer_unref(buffer);
      };

      return absl::make_unique<mediapipe::ImageFrame>(
          mediapipe::ImageFormat::SRGB, meta->width, meta->height,
          meta->stride[0], map.data, deleter);
    }
    return nullptr;
  }

  ~SimpleGstCamera() {
    Stop();
    gst_object_unref(appsink_);
    gst_object_unref(pipeline_);
  }

  static GstFlowReturn OnNewSample(GstAppSink *appsink, SimpleGstCamera *cam) {
    return cam->HandleNewSample(appsink);
  }

private:
  bool HaveBuffer() {
    return buffer_ != nullptr;
  }

  GstFlowReturn HandleNewSample(GstAppSink *appsink) {
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    GstBuffer *new_buffer = gst_sample_get_buffer(sample);

    {
      absl::MutexLock lock(&mutex_);
      if (fps_ == 0.0) {
        // Populate fps_ with info from caps.
        gint fps_n, fps_d;
        GstCaps *caps = gst_sample_get_caps(sample);
        GstStructure *s = gst_caps_get_structure (caps, 0);
        gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d);
        fps_ = double(fps_n) / double(fps_d);
        gst_caps_unref(caps);
      }
      gst_buffer_replace(&buffer_, new_buffer);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  double fps_ = 0.0;
  GstElement *pipeline_ = nullptr;
  GstAppSink *appsink_ = nullptr;
  GstBuffer *buffer_ = nullptr;
  absl::Time last_frame_ = absl::Now();
  absl::Mutex mutex_;
};

::mediapipe::Status RunMPPGraph() {
  std::string calculator_graph_config_contents;
  MP_RETURN_IF_ERROR(mediapipe::file::GetContents(
      FLAGS_calculator_graph_config_file, &calculator_graph_config_contents));
  LOG(INFO) << "Get calculator graph config contents: "
            << calculator_graph_config_contents;
  mediapipe::CalculatorGraphConfig config =
      mediapipe::ParseTextProtoOrDie<mediapipe::CalculatorGraphConfig>(
          calculator_graph_config_contents);

  LOG(INFO) << "Initialize the calculator graph.";
  mediapipe::CalculatorGraph graph;
  MP_RETURN_IF_ERROR(graph.Initialize(config));

  LOG(INFO) << "Initialize the camera or load the video.";
  SimpleGstCamera camera;
  camera.Start();
  LOG(INFO) << "Camera FPS: " << camera.frame_rate();
  // TODO add support for video decode.

  cv::VideoWriter writer;
  const bool save_video = !FLAGS_output_video_path.empty();
  if (save_video) {
    LOG(INFO) << "Prepare video writer.";
    auto test_frame = camera.GetFrame();  // Consume first frame.
    writer.open(FLAGS_output_video_path,
                mediapipe::fourcc('a', 'v', 'c', '1'),  // .mp4
                camera.frame_rate(),cv::Size(test_frame->Width(), test_frame->Height()));
    RET_CHECK(writer.isOpened());
  } else {
    cv::namedWindow(kWindowName, /*flags=WINDOW_AUTOSIZE*/ 1);
  }

  LOG(INFO) << "Start running the calculator graph.";
  ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller poller,
                   graph.AddOutputStreamPoller(kOutputStream));
  MP_RETURN_IF_ERROR(graph.StartRun({}));

  LOG(INFO) << "Start grabbing and processing frames.";
  size_t frame_timestamp = 0;
  bool grab_frames = true;
  while (grab_frames) {
    // Capture flipped RGB camera frame.
    auto input_frame = camera.GetFrame();
    CHECK(input_frame.get()) << "Couldn't get camera frame";  // TODO error handling

    // Send image packet into the graph.
    MP_RETURN_IF_ERROR(graph.AddPacketToInputStream(
        kInputStream, mediapipe::Adopt(input_frame.release())
                          .At(mediapipe::Timestamp(frame_timestamp++))));

    // Get the graph result packet, or stop if that fails.
    mediapipe::Packet packet;
    if (!poller.Next(&packet)) break;
    auto& output_frame = packet.Get<mediapipe::ImageFrame>();

    // Convert back to opencv for display or saving.
    cv::Mat output_frame_mat = mediapipe::formats::MatView(&output_frame);
    cv::cvtColor(output_frame_mat, output_frame_mat, cv::COLOR_RGB2BGR);
    if (save_video) {
      writer.write(output_frame_mat);
    } else {
      cv::imshow(kWindowName, output_frame_mat);
      // Press any key to exit.
      const int pressed_key = cv::waitKey(5);
      if (pressed_key >= 0 && pressed_key != 255) grab_frames = false;
    }
  }

  LOG(INFO) << "Shutting down.";
  if (writer.isOpened()) writer.release();
  MP_RETURN_IF_ERROR(graph.CloseInputStream(kInputStream));
  return graph.WaitUntilDone();
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
