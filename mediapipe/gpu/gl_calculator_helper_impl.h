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

#ifndef MEDIAPIPE_GPU_GL_CALCULATOR_HELPER_IMPL_H_
#define MEDIAPIPE_GPU_GL_CALCULATOR_HELPER_IMPL_H_

#include "mediapipe/gpu/gl_calculator_helper.h"
#include "mediapipe/gpu/gpu_shared_data_internal.h"

#ifdef __OBJC__
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#endif  // __OBJC__

#ifdef __ANDROID__
#include "mediapipe/gpu/gl_texture_buffer_pool.h"
#endif

namespace mediapipe {

// This class implements the GlCalculatorHelper for iOS and Android.
// See GlCalculatorHelper for details on these methods.
class GlCalculatorHelperImpl {
 public:
  explicit GlCalculatorHelperImpl(CalculatorContext* cc,
                                  GpuResources* gpu_resources);
  ~GlCalculatorHelperImpl();

  ::mediapipe::Status RunInGlContext(
      std::function<::mediapipe::Status(void)> gl_func,
      CalculatorContext* calculator_context);

  GlTexture CreateSourceTexture(const ImageFrame& image_frame);
  GlTexture CreateSourceTexture(const GpuBuffer& pixel_buffer);

  // Note: multi-plane support is currently only available on iOS.
  GlTexture CreateSourceTexture(const GpuBuffer& pixel_buffer, int plane);

  // Creates a framebuffer and returns the texture that it is bound to.
  GlTexture CreateDestinationTexture(int output_width, int output_height,
                                     GpuBufferFormat format);

  GLuint framebuffer() const { return framebuffer_; }
  void BindFramebuffer(const GlTexture& dst);

#ifdef __APPLE__
  GlVersion GetGlVersion();
#endif

  GlContext& GetGlContext() const;

  // For internal use.
  void ReadTexture(const GlTexture& texture, void* output, size_t size);

#if HAS_EGL_IMAGE_GBM
  bool CreateEGLImageDMA(int width, int height, GpuBufferFormat format,
      EGLImage *image, int *dma_fd, int *stride);
  void DestroyEGLImageDMA(EGLImage *image, int *dma_fd);
  void MapDMA(int dma_fd, size_t size, void **data);
  void UnmapDMA(void **data, size_t size);
  void BeginCpuAccessDMA(int dma_fd, bool read, bool write);
  void EndCpuAccessDMA(int dma_fd, bool read, bool write);
  void SetEGLSync(EGLSync *sync);
  void WaitEGLSync(EGLSync *sync);
  void DestroyEGLSync(EGLSync *sync);
  void EGLImageTargetTexture2DOES(EGLImage image);
#endif

 private:
  // Makes a GpuBuffer accessible as a texture in the GL context.
  GlTexture MapGpuBuffer(const GpuBuffer& gpu_buffer, int plane);

#if !MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
  GlTexture MapGlTextureBuffer(const GlTextureBufferSharedPtr& texture_buffer);
  GlTextureBufferSharedPtr MakeGlTextureBuffer(const ImageFrame& image_frame);
#endif  // !MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER

  // Sets default texture filtering parameters.
  void SetStandardTextureParams(GLenum target, GLint internal_format);

  // Create the framebuffer for rendering.
  void CreateFramebuffer();

  std::shared_ptr<GlContext> gl_context_;

  GLuint framebuffer_ = 0;

  GpuResources& gpu_resources_;

#if HAS_EGL_IMAGE_GBM
  int drm_fd_ = -1;
  struct gbm_device *gbm_device_ = nullptr;
  bool drm_modifiers_;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_;
#endif
};

}  // namespace mediapipe

#endif  // MEDIAPIPE_GPU_GL_CALCULATOR_HELPER_IMPL_H_
