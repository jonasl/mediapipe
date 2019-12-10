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

#include "absl/strings/str_format.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/port/ret_check.h"
#include "mediapipe/framework/port/status.h"

#define HAVE_GPU_BUFFER
#ifdef __APPLE__
#include "mediapipe/objc/util.h"
#endif

#include "mediapipe/gpu/gl_calculator_helper.h"
#include "mediapipe/gpu/gl_quad_renderer.h"

#if HAS_EGL_IMAGE_GBM
namespace {
  struct DmaTexture {
    EGLImage image = EGL_NO_IMAGE;
    EGLSync sync = EGL_NO_SYNC;
    int dma_fd = 0;
    int stride = 0;
    void *data = nullptr;
    size_t map_size;
    GLuint fb;
    GLuint tex;
    mediapipe::GlCalculatorHelper *helper;

    void SetSync() {
      helper->SetEGLSync(&sync);
    }

    ~DmaTexture() {
      glDeleteTextures(1, &tex);
      glDeleteFramebuffers(1, &fb);
      helper->DestroyEGLSync(&sync);
      helper->UnmapDMA(&data, map_size);
      helper->DestroyEGLImageDMA(&image, &dma_fd);
      LOG(INFO) << "DmaTexture freed";
    }
  };
}
#endif

namespace mediapipe {

// Convert an input image (GpuBuffer or ImageFrame) to ImageFrame.
class GpuBufferToImageFrameCalculator : public CalculatorBase {
 public:
  GpuBufferToImageFrameCalculator() {}

  static ::mediapipe::Status GetContract(CalculatorContract* cc);

  ::mediapipe::Status Open(CalculatorContext* cc) override;
  ::mediapipe::Status Process(CalculatorContext* cc) override;
  ::mediapipe::Status Close(CalculatorContext* cc) override;

 private:
#if !MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
  GlCalculatorHelper helper_;
#if HAS_EGL_IMAGE_GBM
  std::unique_ptr<QuadRenderer> renderer_;
  DmaTexture *dma_texture_ = nullptr;
#endif
#endif  // MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
};
REGISTER_CALCULATOR(GpuBufferToImageFrameCalculator);

// static
::mediapipe::Status GpuBufferToImageFrameCalculator::GetContract(
    CalculatorContract* cc) {
  cc->Inputs().Index(0).SetAny();
  cc->Outputs().Index(0).Set<ImageFrame>();
  // Note: we call this method even on platforms where we don't use the helper,
  // to ensure the calculator's contract is the same. In particular, the helper
  // enables support for the legacy side packet, which several graphs still use.
  MP_RETURN_IF_ERROR(GlCalculatorHelper::UpdateContract(cc));
  return ::mediapipe::OkStatus();
}

::mediapipe::Status GpuBufferToImageFrameCalculator::Open(
    CalculatorContext* cc) {
  // Inform the framework that we always output at the same timestamp
  // as we receive a packet at.
  cc->SetOffset(TimestampDiff(0));
#if !MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
  MP_RETURN_IF_ERROR(helper_.Open(cc));
#endif  // MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
  return ::mediapipe::OkStatus();
}

::mediapipe::Status GpuBufferToImageFrameCalculator::Close(CalculatorContext* cc) {
#if HAS_EGL_IMAGE_GBM
  helper_.RunInGlContext([this]() {
    if (dma_texture_) {
      delete dma_texture_;
      dma_texture_ = nullptr;
    }
  });
#endif
}

::mediapipe::Status GpuBufferToImageFrameCalculator::Process(
    CalculatorContext* cc) {
  if (cc->Inputs().Index(0).Value().ValidateAsType<ImageFrame>().ok()) {
    cc->Outputs().Index(0).AddPacket(cc->Inputs().Index(0).Value());
    return ::mediapipe::OkStatus();
  }

#ifdef HAVE_GPU_BUFFER
  if (cc->Inputs().Index(0).Value().ValidateAsType<GpuBuffer>().ok()) {
    const auto& input = cc->Inputs().Index(0).Get<GpuBuffer>();
#if MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
    std::unique_ptr<ImageFrame> frame =
        CreateImageFrameForCVPixelBuffer(input.GetCVPixelBufferRef());
    cc->Outputs().Index(0).Add(frame.release(), cc->InputTimestamp());
#else
    helper_.RunInGlContext([this, &input, &cc]() {
      auto src = helper_.CreateSourceTexture(input);
#if HAS_EGL_IMAGE_GBM
      // Draw input texture to a dmabuf.
      // TODO: Force RGB (without alpha)?
      // TODO: This is an extra draw, use EGLImage as destination
      // texture upstream instead of this mess.
      if (!renderer_) {
        renderer_ = absl::make_unique<QuadRenderer>();
        renderer_->GlSetup();
      }

      DmaTexture *texture;
      if (dma_texture_) {
        texture = dma_texture_;
        dma_texture_ = nullptr;
      } else {
        texture = new DmaTexture();
        texture->helper = &helper_;
        CHECK(helper_.CreateEGLImageDMA(src.width(), src.height(),
          input.format(), &texture->image, &texture->dma_fd, &texture->stride));
        glGenFramebuffers(1, &texture->fb);
        glBindFramebuffer(GL_FRAMEBUFFER, texture->fb);
        glGenTextures(1, &texture->tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture->tex);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        helper_.EGLImageTargetTexture2DOES(texture->image);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->tex, 0);
        texture->map_size = src.height() * texture->stride;
        helper_.MapDMA(texture->dma_fd, texture->map_size, reinterpret_cast<void**>(&texture->data));
      }

      glBindFramebuffer(GL_FRAMEBUFFER, texture->fb);
      CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

      glViewport(0, 0, src.width(), src.height());
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(src.target(), src.name());
      renderer_->GlRender(
        src.width(), src.height(), src.width(), src.height(), FrameScaleMode::kStretch,
        FrameRotation::kNone, false, false, false);

      texture->SetSync();
      glBindFramebuffer(GL_FRAMEBUFFER, 0);

      auto deleter = [this, texture](uint8*) {
        helper_.RunInGlContext([this, texture]() {
          helper_.EndCpuAccessDMA(texture->dma_fd, true /* read */, false /* write */);
          if (!dma_texture_) {
            // Recycle.
            dma_texture_ = texture;
          } else {
            delete texture;
          }
        });
      };

      std::unique_ptr<ImageFrame> frame = absl::make_unique<mediapipe::ImageFrame>(
          ImageFormatForGpuBufferFormat(input.format()), src.width(),
          src.height(), texture->stride, reinterpret_cast<uint8*>(texture->data), deleter);
      helper_.BeginCpuAccessDMA(texture->dma_fd, true /* read */, false /* write */);
#else
      std::unique_ptr<ImageFrame> frame = absl::make_unique<ImageFrame>(
          ImageFormatForGpuBufferFormat(input.format()), src.width(),
          src.height(), ImageFrame::kGlDefaultAlignmentBoundary);
      helper_.BindFramebuffer(src);
      const auto info = GlTextureInfoForGpuBufferFormat(input.format(), 0);
      glReadPixels(0, 0, src.width(), src.height(), info.gl_format,
                   info.gl_type, frame->MutablePixelData());
      glFlush();
#endif
      cc->Outputs().Index(0).Add(frame.release(), cc->InputTimestamp());
      src.Release();
    });
#endif  // MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
    return ::mediapipe::OkStatus();
  }
#endif  // defined(HAVE_GPU_BUFFER)

  return ::mediapipe::Status(::mediapipe::StatusCode::kInvalidArgument,
                             "Input packets must be ImageFrame or GpuBuffer.");
}

}  // namespace mediapipe
