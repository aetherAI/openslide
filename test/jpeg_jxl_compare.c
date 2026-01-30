/*
 *  OpenSlide, a library for reading whole slide image files
 *
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

/*
 * Test to verify that JPEG and JPEG XL (transcoded from JPEG) produce
 * identical pixel output.
 *
 * Usage: jpeg_jxl_compare <input.jpg>
 *
 * This test:
 * 1. Reads the JPEG file
 * 2. Decodes it with libjpeg (same logic as OpenSlide's jpeg_decode)
 * 3. Converts to JXL using cjxl (must be in PATH)
 * 4. Decodes the JXL with libjxl (same logic as OpenSlide's jpegxl_decode)
 * 5. Compares pixel-by-pixel - they should be identical
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>

#include <openslide.h>
#include "openslide-decode-jpegxl.h"
#include "openslide-decode-jpeg.h"

/*
 * Error handling
 */

static void fail(const char *fmt, ...) G_GNUC_PRINTF(1, 2) G_GNUC_NORETURN;

static void fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "ERROR: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

/*
 * File I/O helpers
 */

static bool read_file(const char *path, uint8_t **out_data, size_t *out_len) {
  gchar *contents = NULL;
  gsize length = 0;
  GError *err = NULL;

  if (!g_file_get_contents(path, &contents, &length, &err)) {
    fprintf(stderr, "Failed to read %s: %s\n", path, err->message);
    g_error_free(err);
    return false;
  }

  *out_data = (uint8_t *) contents;
  *out_len = length;
  return true;
}

/*
 * JPEG decoding
 */

struct jpeg_image {
  uint32_t *pixels;
  int32_t width;
  int32_t height;
};

static bool decode_jpeg(const uint8_t *data, size_t len,
                        struct jpeg_image *out) {
  GError *err = NULL;

  /* Get dimensions */
  if (!_openslide_jpeg_decode_buffer_dimensions(data, (uint32_t) len,
                                                &out->width, &out->height,
                                                &err)) {
    fprintf(stderr, "Failed to get JPEG dimensions: %s\n",
            err ? err->message : "unknown error");
    g_clear_error(&err);
    return false;
  }

  /* Allocate and decode */
  size_t pixel_count = (size_t) out->width * (size_t) out->height;
  out->pixels = g_new(uint32_t, pixel_count);

  if (!_openslide_jpeg_decode_buffer(data, (uint32_t) len,
                                     out->pixels, out->width, out->height,
                                     &err)) {
    fprintf(stderr, "Failed to decode JPEG: %s\n",
            err ? err->message : "unknown error");
    g_clear_error(&err);
    g_free(out->pixels);
    out->pixels = NULL;
    return false;
  }

  return true;
}

/*
 * JPEG to JXL transcoding (using external cjxl tool)
 */

static bool transcode_jpeg_to_jxl(const char *jpeg_path, const char *jxl_path) {
  g_autofree char *cmd = g_strdup_printf(
      "cjxl '%s' '%s' --lossless_jpeg=1 -q 100 2>&1",
      jpeg_path, jxl_path);

  printf("Running: %s\n", cmd);

  int ret = system(cmd);
  if (ret != 0) {
    fprintf(stderr, "cjxl failed with exit code %d\n", ret);
    return false;
  }

  return true;
}

/*
 * JXL decoding
 */

static bool decode_jxl(const uint8_t *data, size_t len,
                       int32_t expected_w, int32_t expected_h,
                       uint32_t **out_pixels) {
  GError *err = NULL;

  size_t pixel_count = (size_t) expected_w * (size_t) expected_h;
  *out_pixels = g_new(uint32_t, pixel_count);

  if (!_openslide_jpegxl_decode_buffer(*out_pixels,
                                       expected_w, expected_h,
                                       data, (int32_t) len,
                                       &err)) {
    fprintf(stderr, "Failed to decode JXL: %s\n",
            err ? err->message : "unknown error");
    g_clear_error(&err);
    g_free(*out_pixels);
    *out_pixels = NULL;
    return false;
  }

  return true;
}

/*
 * Pixel comparison
 */

struct comparison_result {
  size_t total_pixels;
  int diff_count;
  int first_diff_idx;
  int max_channel_diff;
};

static void compare_pixels(const uint32_t *pixels_a, const uint32_t *pixels_b,
                           int32_t w, int32_t h,
                           struct comparison_result *result) {
  size_t pixel_count = (size_t) w * (size_t) h;

  result->total_pixels = pixel_count;
  result->diff_count = 0;
  result->first_diff_idx = -1;
  result->max_channel_diff = 0;

  for (size_t i = 0; i < pixel_count; i++) {
    uint32_t p1 = pixels_a[i];
    uint32_t p2 = pixels_b[i];

    if (p1 != p2) {
      if (result->first_diff_idx == -1) {
        result->first_diff_idx = (int) i;
      }
      result->diff_count++;

      /* Track max channel difference */
      for (int c = 0; c < 4; c++) {
        int v1 = (p1 >> (c * 8)) & 0xff;
        int v2 = (p2 >> (c * 8)) & 0xff;
        int d = abs(v1 - v2);
        if (d > result->max_channel_diff) {
          result->max_channel_diff = d;
        }
      }
    }
  }
}

static void print_pixel_details(const char *label, uint32_t pixel) {
  printf("  %s: 0x%08x (R=%d G=%d B=%d A=%d)\n",
         label, pixel,
         (pixel >> 16) & 0xff,
         (pixel >> 8) & 0xff,
         pixel & 0xff,
         (pixel >> 24) & 0xff);
}

static void report_comparison(const struct comparison_result *result,
                              const uint32_t *pixels_a,
                              const uint32_t *pixels_b,
                              int32_t w) {
  if (result->diff_count == 0) {
    printf("\n*** SUCCESS: All %zu pixels are identical! ***\n",
           result->total_pixels);
    return;
  }

  printf("\n*** FAILURE: %d pixels differ out of %zu (%.2f%%) ***\n",
         result->diff_count, result->total_pixels,
         100.0 * result->diff_count / result->total_pixels);

  /* Show first difference details */
  int idx = result->first_diff_idx;
  int x = idx % w;
  int y = idx / w;

  printf("First difference at pixel %d (%d, %d):\n", idx, x, y);
  print_pixel_details("JPEG", pixels_a[idx]);
  print_pixel_details("JXL ", pixels_b[idx]);
  printf("Max channel difference: %d\n", result->max_channel_diff);
}

/*
 * Main test flow
 */

static int run_comparison(const char *jpeg_path,
                          const struct jpeg_image *jpeg,
                          const char *jxl_path) {
  /* Transcode JPEG to JXL */
  if (!transcode_jpeg_to_jxl(jpeg_path, jxl_path)) {
    return 1;
  }
  printf("Converted JPEG to JXL\n");

  /* Read JXL file */
  g_autofree uint8_t *jxl_data = NULL;
  size_t jxl_len = 0;
  if (!read_file(jxl_path, &jxl_data, &jxl_len)) {
    return 1;
  }
  printf("Read JXL file: %zu bytes\n", jxl_len);

  /* Decode JXL */
  g_autofree uint32_t *jxl_pixels = NULL;
  if (!decode_jxl(jxl_data, jxl_len, jpeg->width, jpeg->height, &jxl_pixels)) {
    return 1;
  }
  printf("Decoded JXL: %dx%d\n", jpeg->width, jpeg->height);

  /* Compare pixels */
  struct comparison_result result;
  compare_pixels(jpeg->pixels, jxl_pixels, jpeg->width, jpeg->height, &result);
  report_comparison(&result, jpeg->pixels, jxl_pixels, jpeg->width);

  return (result.diff_count == 0) ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <input.jpg>\n", argv[0]);
    return 1;
  }

  const char *jpeg_path = argv[1];

  /* Read and decode JPEG */
  g_autofree uint8_t *jpeg_data = NULL;
  size_t jpeg_len = 0;
  if (!read_file(jpeg_path, &jpeg_data, &jpeg_len)) {
    fail("Failed to read JPEG file");
  }
  printf("Read JPEG file: %s (%zu bytes)\n", jpeg_path, jpeg_len);

  struct jpeg_image jpeg = {0};
  if (!decode_jpeg(jpeg_data, jpeg_len, &jpeg)) {
    fail("Failed to decode JPEG");
  }
  printf("Decoded JPEG: %dx%d\n", jpeg.width, jpeg.height);

  /* Create temp file for JXL */
  g_autofree char *jxl_path = NULL;
  GError *err = NULL;
  int fd = g_file_open_tmp("test_XXXXXX.jxl", &jxl_path, &err);
  if (fd == -1) {
    fail("Failed to create temp file: %s", err->message);
  }
  close(fd);
  printf("Temp JXL file: %s\n", jxl_path);

  /* Run comparison */
  int exit_code = run_comparison(jpeg_path, &jpeg, jxl_path);

  /* Cleanup */
  unlink(jxl_path);
  g_free(jpeg.pixels);

  return exit_code;
}

