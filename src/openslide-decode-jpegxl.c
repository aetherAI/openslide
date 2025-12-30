/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2015 Benjamin Gilbert
 *  Copyright (c) 2025 Tsung-Ju Lii
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

static const char *jxl_signature_name(JxlSignature sig) {
  switch (sig) {
  case JXL_SIG_NOT_ENOUGH_BYTES:
    return "JXL_SIG_NOT_ENOUGH_BYTES";
  case JXL_SIG_INVALID:
    return "JXL_SIG_INVALID";
  case JXL_SIG_CODESTREAM:
    return "JXL_SIG_CODESTREAM";
  case JXL_SIG_CONTAINER:
    return "JXL_SIG_CONTAINER";
  default:
    return "JXL_SIG_UNKNOWN";
  }
}

static void format_hex_prefix(char *out, size_t outlen,
                              const uint8_t *data, size_t datalen,
                              size_t max_bytes) {
  g_assert(out != NULL);
  if (outlen == 0) {
    return;
  }
  out[0] = '\0';
  if (!data || datalen == 0) {
    g_strlcpy(out, "<empty>", outlen);
    return;
  }

  size_t n = MIN(datalen, max_bytes);
  size_t pos = 0;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      int written = g_snprintf(out + pos, outlen - pos, " %02x", data[i]);
      if (written < 0 || (size_t) written >= outlen - pos) {
        return;
      }
      pos += (size_t) written;
    } else {
      int written = g_snprintf(out + pos, outlen - pos, "%02x", data[i]);
      if (written < 0 || (size_t) written >= outlen - pos) {
        return;
      }
      pos += (size_t) written;
    }
  }
}

static const char *guess_payload_hint(const uint8_t *data, size_t datalen) {
  if (!data || datalen < 2) {
    return "payload too short";
  }
  // JPEG SOI
  if (data[0] == 0xff && data[1] == 0xd8) {
    return "payload looks like JPEG (starts with FF D8)";
  }
  // JPEG XL codestream signature: 0xFF 0x0A
  if (data[0] == 0xff && data[1] == 0x0a) {
    return "payload looks like JPEG XL codestream (starts with FF 0A)";
  }
  // JPEG XL container signature: 00 00 00 0C 4A 58 4C 20 0D 0A 87 0A
  static const uint8_t jxl_container_sig[12] = {
    0x00, 0x00, 0x00, 0x0c, 0x4a, 0x58, 0x4c, 0x20, 0x0d, 0x0a, 0x87, 0x0a,
  };
  if (datalen >= sizeof(jxl_container_sig) &&
      memcmp(data, jxl_container_sig, sizeof(jxl_container_sig)) == 0) {
    return "payload looks like JPEG XL container (JXL \\r\\n\\x87\\n)";
  }
  return "unrecognized payload prefix";
}

bool _openslide_jpegxl_decode_buffer(uint32_t *dest,
                                     int32_t w, int32_t h,
                                     const void *data, int32_t datalen,
                                     GError **err) {
  g_assert(dest != NULL);
  g_assert(data != NULL);
  g_assert(datalen >= 0);

  // Validate signature early for clearer errors.
  const size_t sig_len = MIN((size_t) datalen, (size_t) 16);
  JxlSignature signature = JxlSignatureCheck((const uint8_t *) data, sig_len);
  if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER) {
    char prefix[3 * 16];
    format_hex_prefix(prefix, sizeof(prefix), (const uint8_t *) data,
                      (size_t) datalen, 16);
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a JPEG XL codestream (signature=%s, checked=%zu, datalen=%d, prefix=%s; %s)",
                jxl_signature_name(signature), sig_len, datalen, prefix,
                guess_payload_hint((const uint8_t *) data, (size_t) datalen));
    return false;
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
                               JXL_DEC_JPEG_RECONSTRUCTION |
                               JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
    return set_jxl_error(err, "JxlDecoderSubscribeEvents() failed");
  }

  if (JxlDecoderSetInput(dec, (const uint8_t *) data, (size_t) datalen) !=
      JXL_DEC_SUCCESS) {
    return set_jxl_error(err, "JxlDecoderSetInput() failed");
  }
  // Keep the input open (imagecodecs does this) even though we have the full
  // buffer; both modes work, but keeping it open matches upstream logic.

  bool have_basic_info = false;
  JxlBasicInfo info;
  memset(&info, 0, sizeof(info));

  JxlPixelFormat format;
  memset(&format, 0, sizeof(format));
  format.data_type = JXL_TYPE_UINT8;
  format.endianness = JXL_NATIVE_ENDIAN;
  format.align = 0;

  // Number of interleaved channels requested from libjxl.
  uint32_t samples = 0;

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

      // Mirror the channel selection behavior in imagecodecs.jpegxl_decode:
      // - L (1 sample): grayscale
      // - RGB: 3 channels
      // - LA/RGBA: include alpha channel in decode buffer, then ignore it
      // - RGB + other extra channels: ignore extras
      // - L + extra channels (no alpha): treat as grayscale and ignore extras
      samples = info.num_color_channels + info.num_extra_channels;
      if (samples == 1) {
        samples = 1;
      } else if (info.num_extra_channels == 0) {
        samples = info.num_color_channels;
      } else if (info.alpha_bits > 0) {
        samples = info.num_color_channels + 1;
      } else if (info.num_color_channels != 1) {
        samples = info.num_color_channels;
      } else {
        // L + C (non-alpha extra channels): OpenSlide only supports RGB/gray.
        samples = 1;
      }
      if (samples != 1 && samples != 2 && samples != 3 && samples != 4) {
        return set_jxl_error(err, "Unsupported JPEG XL channel layout");
      }
      format.num_channels = samples;
      break;
    case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
      if (!have_basic_info) {
        return set_jxl_error(err, "JPEG XL decoder requested output buffer before BASIC_INFO");
      }
      if (JxlDecoderImageOutBufferSize(dec, &format, &pixels_size) !=
          JXL_DEC_SUCCESS) {
        return set_jxl_error(err, "JxlDecoderImageOutBufferSize() failed");
      }
      // Sanity-check buffer size when decoding to 8-bit interleaved pixels.
      if (pixels_size != (size_t) w * (size_t) h * (size_t) format.num_channels) {
        return set_jxl_error(err, "Unexpected JPEG XL output buffer size");
      }
      pixels = g_malloc(pixels_size);
      if (JxlDecoderSetImageOutBuffer(dec, &format, pixels, pixels_size) !=
          JXL_DEC_SUCCESS) {
        return set_jxl_error(err, "JxlDecoderSetImageOutBuffer() failed");
      }
      break;
    case JXL_DEC_JPEG_RECONSTRUCTION:
      // If the bitstream originated from JPEG lossless transcode, libjxl can
      // emit JPEG reconstruction data. We intentionally ignore it and request
      // pixels via the image out buffer.
      break;
    case JXL_DEC_FULL_IMAGE:
      if (!pixels) {
        return set_jxl_error(err, "JPEG XL decode succeeded without output buffer");
      }
      if (format.num_channels == 1 || format.num_channels == 2) {
        // L or LA
        const uint8_t *p = pixels;
        for (int32_t i = 0; i < w * h; i++, p += format.num_channels) {
          write_pixel_rgb(dest++, p[0], p[0], p[0]);
        }
      } else {
        // RGB or RGBA
        const uint8_t *p = pixels;
        for (int32_t i = 0; i < w * h; i++, p += format.num_channels) {
          write_pixel_rgb(dest++, p[0], p[1], p[2]);
        }
      }
      return true;
    case JXL_DEC_SUCCESS:
      // End of codestream. If we haven't seen a full image, treat as error.
      return set_jxl_error(err, "JPEG XL decode finished without image");
    default:
      return set_jxl_error(err, "Unexpected JPEG XL decoder status");
    }
  }
}
