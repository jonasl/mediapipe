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

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <thread>

#include "absl/memory/memory.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/port/commandlineflags.h"
#include "mediapipe/framework/port/status.h"

class SimpleGstCamera
{
public:

  SimpleGstCamera() {
    gst_init (nullptr, nullptr);

    // TODO: This needs to be configurable, support MJPEG/h264 cameras,
    // v4l2 device selection etc. For now use gst_parse_launch for convenience.
    pipeline_ = gst_parse_launch(
      "v4l2src ! video/x-raw,width=1920,height=1080 ! "
      " glfilterbin filter=\"glvideoflip video-direction=horiz ! glcolorscale\" !"
      " video/x-raw,format=RGB ! appsink name=appsink",
      /* TODO: pass in and parse GError */ NULL);
    GstAppSink *appsink = GST_APP_SINK(gst_bin_get_by_name (GST_BIN(pipeline_), "appsink"));
    g_object_set(G_OBJECT(appsink), "emit-signals", TRUE, "sync", FALSE, nullptr);
    g_signal_connect (appsink, "new-sample",
        G_CALLBACK (SimpleGstCamera::OnNewSample), this);
    gst_object_unref(appsink);
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

  std::unique_ptr<mediapipe::ImageFrame> GetFrame(uint32_t timeout_ms) {
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
      // Copy the frame.
      // TODO: No copies, ever. Need to be able to wrap GstBuffer in ImageFrame.
      // TODO: Also return timestamp.
      // TODO: convert from stride to mediapipe alignment. For now just support
      // alignment 1, i.e. (width % stride) == 0. Coral Dev Board GPU will always
      // use alignment 1 for RGB, GStreamers CPU elements uses alignment 4 for RGB.
      double since_last_ms = absl::ToDoubleMilliseconds(absl::Now() - last_frame_);
      last_frame_ = absl::Now();

      GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
      LOG(INFO) << absl::StrFormat("Frame: %ux%u, stride %d, ts %" GST_TIME_FORMAT
        " since_last %.2f ms (%.2f fps)",
        meta->width, meta->height, meta->stride[0],
        GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buffer)),
        since_last_ms, 1000 / since_last_ms);

      auto frame = absl::make_unique<mediapipe::ImageFrame>(
          mediapipe::ImageFormat::SRGB, meta->width, meta->height,
          1 /* tightly packed */);

      // Map buffer for reading.
      GstMapInfo map;
      CHECK(gst_buffer_map(buffer, &map, GST_MAP_READ));
      CHECK_GE(frame->PixelDataSize(), map.size);

      // Copy the pixels.
      // TODO: Stop crying.
      memcpy(frame->MutablePixelData(), map.data, map.size);

      gst_buffer_unmap(buffer, &map);
      gst_buffer_unref(buffer);
      return frame;
    }
    return nullptr;
  }

  ~SimpleGstCamera() {
    Stop();
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
      gst_buffer_replace(&buffer_, new_buffer);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  bool error_ = false;
  GstElement *pipeline_ = nullptr;
  GstBuffer *buffer_ = nullptr;
  absl::Time last_frame_ = absl::Now();
  absl::Mutex mutex_;
};

::mediapipe::Status RunGStreamer() {
  LOG(INFO) << "Creating camera pipeline";
  SimpleGstCamera camera;
  LOG(INFO) << "Starting camera";
  camera.Start();

  const uint32_t kFrameCount = 100;
  LOG(INFO) << "Capturing " << kFrameCount << " frames...";
  absl::Time start_time = absl::Now();

  for (int i = 0; i < kFrameCount; ++i) {
    auto frame = camera.GetFrame(1000);
    CHECK(frame.get()); // TODO: no error handling here.
  }
  
  double elapsed_s = absl::ToDoubleSeconds(absl::Now() - start_time);
  LOG(INFO) << absl::StrFormat("Captured %u frames in %.2f s, %.2f fps",
    kFrameCount, elapsed_s, kFrameCount / elapsed_s);

  return ::mediapipe::Status();
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  ::mediapipe::Status run_status = RunGStreamer();
  if (!run_status.ok()) {
    LOG(ERROR) << "Failed to run: " << run_status.message();
  } else {
    LOG(INFO) << "Success!";
  }
  return 0;
}
