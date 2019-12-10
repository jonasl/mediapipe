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

#include "mediapipe/gpu/gl_calculator_helper_impl.h"
#include "mediapipe/gpu/gpu_buffer_format.h"
#include "mediapipe/gpu/gpu_shared_data_internal.h"

#if HAS_EGL_IMAGE_GBM
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#define HANDLE_EINTR(x)                                           \
  ({                                                              \
    int eintr_wrapper_result;                                     \
    do {                                                          \
      eintr_wrapper_result = (x);                                 \
    } while (eintr_wrapper_result == -1 && errno == EINTR);       \
    eintr_wrapper_result;                                         \
  })
// Avoid dependency on libdrm for this define only.
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR (uint64_t)0
#endif
#endif

namespace mediapipe {

GlCalculatorHelperImpl::GlCalculatorHelperImpl(CalculatorContext* cc,
                                               GpuResources* gpu_resources)
    : gpu_resources_(*gpu_resources) {
  gl_context_ = gpu_resources_.gl_context(cc);
#if HAS_EGL_IMAGE_GBM
  drm_fd_ = open("/dev/dri/renderD128", O_RDWR);
  CHECK_NE(drm_fd_, -1) << "Failed to open DRM render node";
  gbm_device_ = gbm_create_device(drm_fd_);
  CHECK(gbm_device_) << "Failed to create GBM device";
  drm_modifiers_ = strstr(eglQueryString(gl_context_->egl_display(), EGL_EXTENSIONS),
      "EGL_EXT_image_dma_buf_import_modifiers");
  glEGLImageTargetTexture2DOES_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  CHECK(glEGLImageTargetTexture2DOES_) << "glEGLImageTargetTexture2DOES unsupported";
#endif
}

GlCalculatorHelperImpl::~GlCalculatorHelperImpl() {
#if HAS_EGL_IMAGE_GBM
  if (gbm_device_) {
    gbm_device_destroy(gbm_device_);
  }
  if (drm_fd_ > 0) {
    close(drm_fd_);
  }
#endif
  RunInGlContext(
      [this] {
        if (framebuffer_) {
          glDeleteFramebuffers(1, &framebuffer_);
          framebuffer_ = 0;
        }
        return ::mediapipe::OkStatus();
      },
      /*calculator_context=*/nullptr)
      .IgnoreError();
}

GlContext& GlCalculatorHelperImpl::GetGlContext() const { return *gl_context_; }

::mediapipe::Status GlCalculatorHelperImpl::RunInGlContext(
    std::function<::mediapipe::Status(void)> gl_func,
    CalculatorContext* calculator_context) {
  if (calculator_context) {
    return gl_context_->Run(std::move(gl_func), calculator_context->NodeId(),
                            calculator_context->InputTimestamp());
  } else {
    return gl_context_->Run(std::move(gl_func));
  }
}

void GlCalculatorHelperImpl::CreateFramebuffer() {
  // Our framebuffer will have a color attachment but no depth attachment,
  // so it's important that the depth test be off. It is disabled by default,
  // but we wanted to be explicit.
  // TODO: move this to glBindFramebuffer?
  glDisable(GL_DEPTH_TEST);
  glGenFramebuffers(1, &framebuffer_);
}

void GlCalculatorHelperImpl::BindFramebuffer(const GlTexture& dst) {
#ifdef __ANDROID__
  // On (some?) Android devices, attaching a new texture to the frame buffer
  // does not seem to detach the old one. As a result, using that texture
  // for texturing can produce incorrect output. See b/32091368 for details.
  // To fix this, we have to call either glBindFramebuffer with a FBO id of 0
  // or glFramebufferTexture2D with a texture ID of 0.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
  if (!framebuffer_) {
    CreateFramebuffer();
  }
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glViewport(0, 0, dst.width(), dst.height());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(dst.target(), dst.name());
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst.target(),
                         dst.name(), 0);

#ifndef NDEBUG
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    VLOG(2) << "incomplete framebuffer: " << status;
  }
#endif
}

void GlCalculatorHelperImpl::SetStandardTextureParams(GLenum target,
                                                      GLint internal_format) {
  GLint filter;
  switch (internal_format) {
    case GL_R32F:
    case GL_RGBA32F:
      // 32F (unlike 16f) textures do not support texture filtering
      // (According to OpenGL ES specification [TEXTURE IMAGE SPECIFICATION])
      filter = GL_NEAREST;
      break;
    default:
      filter = GL_LINEAR;
  }
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

#if !MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER
GlTexture GlCalculatorHelperImpl::CreateSourceTexture(
    const ImageFrame& image_frame) {
  GlTexture texture = MapGlTextureBuffer(MakeGlTextureBuffer(image_frame));
  texture.for_reading_ = true;
  return texture;
}

GlTexture GlCalculatorHelperImpl::CreateSourceTexture(
    const GpuBuffer& gpu_buffer) {
  GlTexture texture = MapGpuBuffer(gpu_buffer, 0);
  texture.for_reading_ = true;
  return texture;
}

GlTexture GlCalculatorHelperImpl::CreateSourceTexture(
    const GpuBuffer& gpu_buffer, int plane) {
  GlTexture texture = MapGpuBuffer(gpu_buffer, plane);
  texture.for_reading_ = true;
  return texture;
}

GlTexture GlCalculatorHelperImpl::MapGpuBuffer(const GpuBuffer& gpu_buffer,
                                               int plane) {
  CHECK_EQ(plane, 0);
  return MapGlTextureBuffer(gpu_buffer.GetGlTextureBufferSharedPtr());
}

GlTexture GlCalculatorHelperImpl::MapGlTextureBuffer(
    const GlTextureBufferSharedPtr& texture_buffer) {
  // Insert wait call to sync with the producer.
  texture_buffer->WaitOnGpu();
  GlTexture texture;
  texture.helper_impl_ = this;
  texture.gpu_buffer_ = GpuBuffer(texture_buffer);
  texture.plane_ = 0;
  texture.width_ = texture_buffer->width_;
  texture.height_ = texture_buffer->height_;
  texture.target_ = texture_buffer->target_;
  texture.name_ = texture_buffer->name_;

  // TODO: do the params need to be reset here??
  glBindTexture(texture.target(), texture.name());
  GlTextureInfo info =
      GlTextureInfoForGpuBufferFormat(texture_buffer->format(), texture.plane_);
  SetStandardTextureParams(texture.target(), info.gl_internal_format);
  glBindTexture(texture.target(), 0);

  return texture;
}

GlTextureBufferSharedPtr GlCalculatorHelperImpl::MakeGlTextureBuffer(
    const ImageFrame& image_frame) {
  CHECK(gl_context_->IsCurrent());
  auto buffer = GlTextureBuffer::Create(
      image_frame.Width(), image_frame.Height(),
      GpuBufferFormatForImageFormat(image_frame.Format()),
      image_frame.PixelData());
  glBindTexture(GL_TEXTURE_2D, buffer->name_);
  GlTextureInfo info =
      GlTextureInfoForGpuBufferFormat(buffer->format_, /*plane=*/0);
  SetStandardTextureParams(buffer->target_, info.gl_internal_format);
  glBindTexture(GL_TEXTURE_2D, 0);

  return buffer;
}
#endif  // !MEDIAPIPE_GPU_BUFFER_USE_CV_PIXEL_BUFFER

GlTexture GlCalculatorHelperImpl::CreateDestinationTexture(
    int width, int height, GpuBufferFormat format) {
  if (!framebuffer_) {
    CreateFramebuffer();
  }

  GpuBuffer buffer =
      gpu_resources_.gpu_buffer_pool().GetBuffer(width, height, format);
  GlTexture texture = MapGpuBuffer(buffer, 0);

  return texture;
}

void GlCalculatorHelperImpl::ReadTexture(const GlTexture& texture, void* output,
                                         size_t size) {
  CHECK_GE(size, texture.width_ * texture.height_ * 4);

  GLint current_fbo;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);
  CHECK_NE(current_fbo, 0);

  GLint color_attachment_name;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                        &color_attachment_name);
  if (color_attachment_name != texture.name_) {
    // Save the viewport. Note that we assume that the color attachment is a
    // GL_TEXTURE_2D texture.
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    // Set the data from GLTexture object.
    glViewport(0, 0, texture.width_, texture.height_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           texture.target_, texture.name_, 0);
    glReadPixels(0, 0, texture.width_, texture.height_, GL_RGBA,
                 GL_UNSIGNED_BYTE, output);

    // Restore from the saved viewport and color attachment name.
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           color_attachment_name, 0);
  } else {
    glReadPixels(0, 0, texture.width_, texture.height_, GL_RGBA,
                 GL_UNSIGNED_BYTE, output);
  }
}

#if HAS_EGL_IMAGE_GBM
bool GlCalculatorHelperImpl::CreateEGLImageDMA(int width, int height,
      GpuBufferFormat format, EGLImage *image, int *dma_fd, int *stride) {
  // TODO: Support more formats?
  uint32_t gbm_format;
  switch (format) {
    case GpuBufferFormat::kBGRA32:
      gbm_format = GBM_FORMAT_ABGR8888;
      break;
    case GpuBufferFormat::kRGB24:
      gbm_format = GBM_FORMAT_BGR888;
      break;
    default:
      CHECK(false) << "Unsupported format for DMA buffer";
      return false;
  }

  CHECK(gbm_device_is_format_supported(gbm_device_, gbm_format, GBM_BO_USE_RENDERING))
      << "GBM impl. doesn't support format " << std::hex << gbm_format;

  struct gbm_bo *bo = gbm_bo_create(gbm_device_, width, height, gbm_format, GBM_BO_USE_RENDERING);
  CHECK(bo) << "Failed to create GBM buffer object";
  uint32_t _stride = gbm_bo_get_stride(bo);

  // Export dmabuf. We now own this fd and must close it when we're done.
  // Once exported we can destroy the bo.
  int fd = gbm_bo_get_fd(bo);
  gbm_bo_destroy(bo);
  CHECK_GE(fd, 0) << "Failed to export dmabuf";

  // Create EGLImage.
  int acnt = 0;
  EGLAttrib attribs[32];
  attribs[acnt++] = EGL_WIDTH;
  attribs[acnt++] = width;
  attribs[acnt++] = EGL_HEIGHT;
  attribs[acnt++] = height;
  attribs[acnt++] = EGL_LINUX_DRM_FOURCC_EXT;
  attribs[acnt++] = gbm_format;

  attribs[acnt++] = EGL_DMA_BUF_PLANE0_FD_EXT;
  attribs[acnt++] = fd;  // Does not take ownership of fd.
  attribs[acnt++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  attribs[acnt++] = 0;
  attribs[acnt++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
  attribs[acnt++] = _stride;
  if (drm_modifiers_) {
    attribs[acnt++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attribs[acnt++] = DRM_FORMAT_MOD_LINEAR & 0xffffffff;
    attribs[acnt++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attribs[acnt++] = (DRM_FORMAT_MOD_LINEAR >> 32) & 0xffffffff;
  }
  attribs[acnt] = EGL_NONE;

  EGLImage _image = eglCreateImage(gl_context_->egl_display(), EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
  CHECK_NE(_image, EGL_NO_IMAGE_KHR) << "Failed to create EGLImage";

  *image = _image;
  *dma_fd = fd;
  *stride = _stride;
  return true;
}

void GlCalculatorHelperImpl::DestroyEGLImageDMA(EGLImage *image, int *dma_fd) {
  if (*image != EGL_NO_IMAGE) {
    eglDestroyImage(gl_context_->egl_display(), *image);
    *image = EGL_NO_IMAGE;
  }
  if (*dma_fd > 0) {
    close(*dma_fd);
    *dma_fd = -1;
  }
}

void GlCalculatorHelperImpl::MapDMA(int dma_fd, size_t size, void **data) {
  *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd, 0);
  CHECK_NE(*data, MAP_FAILED) << "Failed to mmap dmabuf: " << errno;
}

void GlCalculatorHelperImpl::UnmapDMA(void **data, size_t size) {
  if (*data) {
    int res = munmap(*data, size);
    CHECK_EQ(res, 0) << "Failed to munmap dmabuf: " << errno;
    *data = nullptr;
  }
}

void GlCalculatorHelperImpl::BeginCpuAccessDMA(int dma_fd, bool read, bool write) {
  struct dma_buf_sync sync_start = { 0 };
  sync_start.flags = DMA_BUF_SYNC_START;
  if (read) {
    sync_start.flags |= DMA_BUF_SYNC_READ;
  }
  if (write) {
    sync_start.flags |= DMA_BUF_SYNC_WRITE;
  }
  CHECK_EQ(HANDLE_EINTR(ioctl(dma_fd, DMA_BUF_IOCTL_SYNC, &sync_start)), 0);
}

void GlCalculatorHelperImpl::EndCpuAccessDMA(int dma_fd, bool read, bool write) {
  struct dma_buf_sync sync_end = { 0 };
  sync_end.flags = DMA_BUF_SYNC_END;
  if (read) {
    sync_end.flags |= DMA_BUF_SYNC_READ;
  }
  if (write) {
    sync_end.flags |= DMA_BUF_SYNC_WRITE;
  }
  CHECK_EQ(HANDLE_EINTR(ioctl(dma_fd, DMA_BUF_IOCTL_SYNC, &sync_end)), 0);
}

void GlCalculatorHelperImpl::SetEGLSync(EGLSync *sync) {
  DestroyEGLSync(sync);
  *sync = eglCreateSync(gl_context_->egl_display(), EGL_SYNC_FENCE, NULL);
  CHECK_NE(*sync, EGL_NO_SYNC);
}

void GlCalculatorHelperImpl::WaitEGLSync(EGLSync *sync) {
  if (*sync != EGL_NO_SYNC) {
    EGLint res;
    do {
      res = eglClientWaitSync(gl_context_->egl_display(),
          *sync, EGL_SYNC_FLUSH_COMMANDS_BIT, 1000000000 /* 1s */ );
    } while (res == EGL_TIMEOUT_EXPIRED);
  }
}

void GlCalculatorHelperImpl::DestroyEGLSync(EGLSync *sync) {
  if (*sync != EGL_NO_SYNC) {
    eglDestroySync(gl_context_->egl_display(), *sync);
    *sync = EGL_NO_SYNC;
  }
}

void GlCalculatorHelperImpl::EGLImageTargetTexture2DOES(EGLImage image) {
  glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, image);
  CHECK_EQ(glGetError(), GL_NO_ERROR) << "glEGLImageTargetTexture2DOES failed";
}
#endif

}  // namespace mediapipe
