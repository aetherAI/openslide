/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2015 Benjamin Gilbert
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "openslide-private.h"
#include "openslide-decode-jpegxl.h"

#include <string.h>

#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(JxlDecoder, JxlDecoderDestroy)
typedef void JxlThreadParallelRunnerOpaque;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(JxlThreadParallelRunnerOpaque, JxlThreadParallelRunnerDestroy)

static inline void write_pixel_rgb(uint32_t *dest,
                                   uint8_t r, uint8_t g, uint8_t b) {
  *dest = 0xff000000 | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
}

static bool set_jxl_error(GError **err, const char *msg) {
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "%s", msg);
  return false;
}

bool _openslide_jpegxl_decode_buffer(uint32_t *dest,
                                     int32_t w, int32_t h,
                                     const void *data, int32_t datalen,
                                     enum _openslide_jpegxl_colorspace space,
                                     GError **err) {
  g_assert(dest != NULL);
  g_assert(data != NULL);
  g_assert(datalen >= 0);

  if (space != OPENSLIDE_JPEGXL_SRGB) {
    return set_jxl_error(err, "Unsupported JPEG XL colorspace");
  }

  g_autoptr(JxlDecoder) dec = JxlDecoderCreate(NULL);
  if (!dec) {
    return set_jxl_error(err, "JxlDecoderCreate() failed");
  }

  const size_t threads = JxlThreadParallelRunnerDefaultNumWorkerThreads();
  g_autoptr(JxlThreadParallelRunnerOpaque) runner =
      JxlThreadParallelRunnerCreate(NULL, threads);
  if (!runner) {
    return set_jxl_error(err, "JxlThreadParallelRunnerCreate() failed");
  }
  if (JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner) !=
      JXL_DEC_SUCCESS) {
    return set_jxl_error(err, "JxlDecoderSetParallelRunner() failed");
  }

  if (JxlDecoderSubscribeEvents(dec,
                               JXL_DEC_BASIC_INFO |
                               JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
    return set_jxl_error(err, "JxlDecoderSubscribeEvents() failed");
  }

  if (JxlDecoderSetInput(dec, (const uint8_t *) data, (size_t) datalen) !=
      JXL_DEC_SUCCESS) {
    return set_jxl_error(err, "JxlDecoderSetInput() failed");
  }
  JxlDecoderCloseInput(dec);

  bool have_basic_info = false;
  JxlBasicInfo info;
  memset(&info, 0, sizeof(info));

  JxlPixelFormat format;
  memset(&format, 0, sizeof(format));
  format.data_type = JXL_TYPE_UINT8;
  format.endianness = JXL_NATIVE_ENDIAN;
  format.align = 0;

  g_autofree uint8_t *pixels = NULL;
  size_t pixels_size = 0;

  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(dec);
    switch (status) {
    case JXL_DEC_ERROR:
      return set_jxl_error(err, "JPEG XL decode failed");
    case JXL_DEC_NEED_MORE_INPUT:
      return set_jxl_error(err, "Truncated JPEG XL input");
    case JXL_DEC_BASIC_INFO:
      if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
        return set_jxl_error(err, "JxlDecoderGetBasicInfo() failed");
      }
      have_basic_info = true;
      if (info.xsize != (uint32_t) w || info.ysize != (uint32_t) h) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Dimensional mismatch reading JPEG XL, expected %dx%d, got %ux%u",
                    w, h, info.xsize, info.ysize);
        return false;
      }
      format.num_channels = info.num_color_channels == 1 ? 1 : 3;
      break;
    case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
      if (!have_basic_info) {
        return set_jxl_error(err, "JPEG XL decoder requested output buffer before BASIC_INFO");
      }
      if (JxlDecoderImageOutBufferSize(dec, &format, &pixels_size) !=
          JXL_DEC_SUCCESS) {
        return set_jxl_error(err, "JxlDecoderImageOutBufferSize() failed");
      }
      pixels = g_malloc(pixels_size);
      if (JxlDecoderSetImageOutBuffer(dec, &format, pixels, pixels_size) !=
          JXL_DEC_SUCCESS) {
        return set_jxl_error(err, "JxlDecoderSetImageOutBuffer() failed");
      }
      break;
    case JXL_DEC_FULL_IMAGE:
      break;
    case JXL_DEC_SUCCESS:
      if (!pixels) {
        return set_jxl_error(err, "JPEG XL decode succeeded without output buffer");
      }
      if (format.num_channels == 1) {
        const uint8_t *p = pixels;
        for (int32_t i = 0; i < w * h; i++, p++) {
          write_pixel_rgb(dest++, *p, *p, *p);
        }
      } else {
        const uint8_t *p = pixels;
        for (int32_t i = 0; i < w * h; i++, p += 3) {
          write_pixel_rgb(dest++, p[0], p[1], p[2]);
        }
      }
      return true;
    default:
      return set_jxl_error(err, "Unexpected JPEG XL decoder status");
    }
  }
}
