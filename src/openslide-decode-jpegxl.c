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
#include "openslide-decode-jpeg.h"

#include <string.h>

#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(JxlDecoder, JxlDecoderDestroy)
typedef void JxlThreadParallelRunnerOpaque;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(JxlThreadParallelRunnerOpaque, JxlThreadParallelRunnerDestroy)

/*
 * Decoder context - holds all state for a single decode operation.
 * Uses g_auto for automatic cleanup of allocated buffers.
 */
typedef struct {
  /* Output parameters */
  uint32_t *dest;
  int32_t w;
  int32_t h;

  /* Decoder state */
  JxlDecoder *dec;
  JxlBasicInfo info;
  JxlPixelFormat format;
  bool have_basic_info;

  /* Pixel buffer for native JXL decoding (owned, will be freed) */
  uint8_t *pixels;
  size_t pixels_size;

  /* JPEG reconstruction state (owned, will be freed) */
  uint8_t *jpeg_buf;
  size_t jpeg_buf_size;
  size_t jpeg_written;
  bool jpeg_reconstruction_active;
} JxlDecodeCtx;

static void jxl_decode_ctx_cleanup(JxlDecodeCtx *ctx) {
  g_free(ctx->pixels);
  g_free(ctx->jpeg_buf);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(JxlDecodeCtx, jxl_decode_ctx_cleanup)

/*
 * Helper functions
 */

static inline void write_pixel_rgb(uint32_t *dest,
                                   uint8_t r, uint8_t g, uint8_t b) {
  *dest = 0xff000000 | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
}

static bool set_error(GError **err, const char *msg) {
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
    int written = g_snprintf(out + pos, outlen - pos,
                             i ? " %02x" : "%02x", data[i]);
    if (written < 0 || (size_t) written >= outlen - pos) {
      return;
    }
    pos += (size_t) written;
  }
}

static const char *guess_payload_hint(const uint8_t *data, size_t datalen) {
  if (!data || datalen < 2) {
    return "payload too short";
  }
  if (data[0] == 0xff && data[1] == 0xd8) {
    return "payload looks like JPEG (starts with FF D8)";
  }
  if (data[0] == 0xff && data[1] == 0x0a) {
    return "payload looks like JPEG XL codestream (starts with FF 0A)";
  }
  static const uint8_t jxl_container_sig[12] = {
    0x00, 0x00, 0x00, 0x0c, 0x4a, 0x58, 0x4c, 0x20, 0x0d, 0x0a, 0x87, 0x0a,
  };
  if (datalen >= sizeof(jxl_container_sig) &&
      memcmp(data, jxl_container_sig, sizeof(jxl_container_sig)) == 0) {
    return "payload looks like JPEG XL container (JXL \\r\\n\\x87\\n)";
  }
  return "unrecognized payload prefix";
}

/*
 * Signature validation
 */
static bool validate_jxl_signature(const void *data, int32_t datalen,
                                   GError **err) {
  const size_t sig_len = MIN((size_t) datalen, (size_t) 16);
  JxlSignature signature = JxlSignatureCheck((const uint8_t *) data, sig_len);

  if (signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER) {
    return true;
  }

  char prefix[3 * 16];
  format_hex_prefix(prefix, sizeof(prefix), (const uint8_t *) data,
                    (size_t) datalen, 16);
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "Not a JPEG XL codestream (signature=%s, checked=%zu, "
              "datalen=%d, prefix=%s; %s)",
              jxl_signature_name(signature), sig_len, datalen, prefix,
              guess_payload_hint((const uint8_t *) data, (size_t) datalen));
  return false;
}

/*
 * Channel count selection based on image info.
 * Returns the number of interleaved channels to request from libjxl.
 */
static uint32_t compute_output_channels(const JxlBasicInfo *info) {
  uint32_t samples = info->num_color_channels + info->num_extra_channels;

  if (samples == 1) {
    return 1;  /* Grayscale */
  }
  if (info->num_extra_channels == 0) {
    return info->num_color_channels;  /* RGB without alpha */
  }
  if (info->alpha_bits > 0) {
    return info->num_color_channels + 1;  /* RGB/L + alpha */
  }
  if (info->num_color_channels != 1) {
    return info->num_color_channels;  /* RGB, ignore non-alpha extras */
  }
  return 1;  /* L + non-alpha extras: treat as grayscale */
}

/*
 * Convert decoded pixels to OpenSlide ARGB format.
 */
static void convert_pixels_to_argb(uint32_t *dest, const uint8_t *pixels,
                                   int32_t w, int32_t h, uint32_t num_channels) {
  const size_t pixel_count = (size_t) w * (size_t) h;

  if (num_channels == 1 || num_channels == 2) {
    /* Grayscale or Grayscale + Alpha */
    const uint8_t *p = pixels;
    for (size_t i = 0; i < pixel_count; i++, p += num_channels) {
      write_pixel_rgb(dest++, p[0], p[0], p[0]);
    }
  } else {
    /* RGB or RGBA */
    const uint8_t *p = pixels;
    for (size_t i = 0; i < pixel_count; i++, p += num_channels) {
      write_pixel_rgb(dest++, p[0], p[1], p[2]);
    }
  }
}

/*
 * Event handlers for the decoder state machine.
 * Each handler returns true on success, false on error.
 */

static bool handle_basic_info(JxlDecodeCtx *ctx, GError **err) {
  if (JxlDecoderGetBasicInfo(ctx->dec, &ctx->info) != JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderGetBasicInfo() failed");
  }
  ctx->have_basic_info = true;

  /* Validate dimensions */
  if (ctx->info.xsize != (uint32_t) ctx->w ||
      ctx->info.ysize != (uint32_t) ctx->h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Dimensional mismatch reading JPEG XL, expected %dx%d, got %ux%u",
                ctx->w, ctx->h, ctx->info.xsize, ctx->info.ysize);
    return false;
  }

  /* Configure output format */
  uint32_t samples = compute_output_channels(&ctx->info);
  if (samples != 1 && samples != 2 && samples != 3 && samples != 4) {
    return set_error(err, "Unsupported JPEG XL channel layout");
  }
  ctx->format.num_channels = samples;

  return true;
}

static bool handle_jpeg_reconstruction_start(JxlDecodeCtx *ctx,
                                             int32_t datalen, GError **err) {
  /* Allocate buffer for reconstructed JPEG */
  ctx->jpeg_buf_size = (size_t) datalen * 2;
  if (ctx->jpeg_buf_size < 65536) {
    ctx->jpeg_buf_size = 65536;
  }
  ctx->jpeg_buf = g_malloc(ctx->jpeg_buf_size);

  if (JxlDecoderSetJPEGBuffer(ctx->dec, ctx->jpeg_buf,
                              ctx->jpeg_buf_size) != JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderSetJPEGBuffer() failed");
  }
  ctx->jpeg_reconstruction_active = true;

  return true;
}

static bool handle_jpeg_need_more_output(JxlDecodeCtx *ctx,
                                         GError **err) {
  /* Grow the JPEG reconstruction buffer */
  size_t remaining = JxlDecoderReleaseJPEGBuffer(ctx->dec);
  ctx->jpeg_written = ctx->jpeg_buf_size - remaining;

  size_t new_size = ctx->jpeg_buf_size * 2;
  ctx->jpeg_buf = g_realloc(ctx->jpeg_buf, new_size);
  ctx->jpeg_buf_size = new_size;

  if (JxlDecoderSetJPEGBuffer(ctx->dec, ctx->jpeg_buf + ctx->jpeg_written,
                              ctx->jpeg_buf_size - ctx->jpeg_written) !=
      JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderSetJPEGBuffer() failed after resize");
  }

  return true;
}

static bool handle_need_image_out_buffer(JxlDecodeCtx *ctx,
                                         GError **err) {
  if (!ctx->have_basic_info) {
    return set_error(err,
                     "JPEG XL decoder requested output buffer before BASIC_INFO");
  }

  /* Skip if doing JPEG reconstruction */
  if (ctx->jpeg_reconstruction_active) {
    return true;
  }

  /* Allocate pixel buffer */
  if (JxlDecoderImageOutBufferSize(ctx->dec, &ctx->format,
                                   &ctx->pixels_size) != JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderImageOutBufferSize() failed");
  }

  size_t expected_size = (size_t) ctx->w * (size_t) ctx->h *
                         (size_t) ctx->format.num_channels;
  if (ctx->pixels_size != expected_size) {
    return set_error(err, "Unexpected JPEG XL output buffer size");
  }

  ctx->pixels = g_malloc(ctx->pixels_size);
  if (JxlDecoderSetImageOutBuffer(ctx->dec, &ctx->format, ctx->pixels,
                                  ctx->pixels_size) != JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderSetImageOutBuffer() failed");
  }

  return true;
}

static bool handle_full_image(JxlDecodeCtx *ctx, GError **err) {
  if (ctx->jpeg_reconstruction_active) {
    /* Finalize JPEG reconstruction and decode with libjpeg */
    size_t remaining = JxlDecoderReleaseJPEGBuffer(ctx->dec);
    ctx->jpeg_written = ctx->jpeg_buf_size - remaining;

    return _openslide_jpeg_decode_buffer(ctx->jpeg_buf,
                                         (uint32_t) ctx->jpeg_written,
                                         ctx->dest, ctx->w, ctx->h, err);
  }

  /* Native JXL decode path */
  if (!ctx->pixels) {
    return set_error(err, "JPEG XL decode succeeded without output buffer");
  }

  convert_pixels_to_argb(ctx->dest, ctx->pixels, ctx->w, ctx->h,
                         ctx->format.num_channels);
  return true;
}

/*
 * Process decoder events in a loop.
 * Returns true on success, false on error.
 */
static bool process_decode_events(JxlDecodeCtx *ctx,
                                  int32_t datalen, GError **err) {
  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(ctx->dec);

    switch (status) {
    case JXL_DEC_ERROR:
      return set_error(err, "JPEG XL decode failed");

    case JXL_DEC_NEED_MORE_INPUT:
      return set_error(err, "Truncated JPEG XL input");

    case JXL_DEC_BASIC_INFO:
      if (!handle_basic_info(ctx, err)) {
        return false;
      }
      break;

    case JXL_DEC_JPEG_RECONSTRUCTION:
      if (!handle_jpeg_reconstruction_start(ctx, datalen, err)) {
        return false;
      }
      break;

    case JXL_DEC_JPEG_NEED_MORE_OUTPUT:
      if (!handle_jpeg_need_more_output(ctx, err)) {
        return false;
      }
      break;

    case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
      if (!handle_need_image_out_buffer(ctx, err)) {
        return false;
      }
      break;

    case JXL_DEC_FULL_IMAGE:
      return handle_full_image(ctx, err);

    case JXL_DEC_SUCCESS:
      return set_error(err, "JPEG XL decode finished without image");

    default:
      return set_error(err, "Unexpected JPEG XL decoder status");
    }
  }
}

/*
 * Main decode function
 */
static bool jpegxl_decode(uint32_t *dest, int32_t w, int32_t h,
                          const void *data, int32_t datalen, GError **err) {
  g_assert(dest != NULL);
  g_assert(data != NULL);
  g_assert(datalen >= 0);

  /* Validate signature */
  if (!validate_jxl_signature(data, datalen, err)) {
    return false;
  }

  /* Initialize decoder */
  g_autoptr(JxlDecoder) dec = JxlDecoderCreate(NULL);
  if (!dec) {
    return set_error(err, "JxlDecoderCreate() failed");
  }

  /* Set up parallel runner */
  const size_t threads = JxlThreadParallelRunnerDefaultNumWorkerThreads();
  g_autoptr(JxlThreadParallelRunnerOpaque) runner =
      JxlThreadParallelRunnerCreate(NULL, threads);
  if (!runner) {
    return set_error(err, "JxlThreadParallelRunnerCreate() failed");
  }
  if (JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner) !=
      JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderSetParallelRunner() failed");
  }

  /* Subscribe to events */
  if (JxlDecoderSubscribeEvents(dec,
                                JXL_DEC_BASIC_INFO |
                                JXL_DEC_JPEG_RECONSTRUCTION |
                                JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderSubscribeEvents() failed");
  }

  /* Set input data */
  if (JxlDecoderSetInput(dec, (const uint8_t *) data, (size_t) datalen) !=
      JXL_DEC_SUCCESS) {
    return set_error(err, "JxlDecoderSetInput() failed");
  }

  /* Initialize context with automatic cleanup */
  g_auto(JxlDecodeCtx) ctx = {
    .dest = dest,
    .w = w,
    .h = h,
    .dec = dec,
    .have_basic_info = false,
    .format = {
      .data_type = JXL_TYPE_UINT8,
      .endianness = JXL_NATIVE_ENDIAN,
      .align = 0,
      .num_channels = 0,
    },
    .pixels = NULL,
    .pixels_size = 0,
    .jpeg_buf = NULL,
    .jpeg_buf_size = 0,
    .jpeg_written = 0,
    .jpeg_reconstruction_active = false,
  };

  return process_decode_events(&ctx, datalen, err);
}

bool _openslide_jpegxl_decode_buffer(uint32_t *dest,
                                     int32_t w, int32_t h,
                                     const void *data, int32_t datalen,
                                     GError **err) {
  return jpegxl_decode(dest, w, h, data, datalen, err);
}
