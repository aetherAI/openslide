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
#include <setjmp.h>
#include <glib.h>

#include <jpeglib.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <openslide.h>
#include "openslide-decode-jpegxl.h"
#include "openslide-decode-jpeg.h"

static void fail(const char *fmt, ...) G_GNUC_PRINTF(1, 2) G_GNUC_NORETURN;

static void fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

/* Read entire file into memory */
static bool read_file(const char *path, uint8_t **out_data, size_t *out_len) {
  gchar *contents = NULL;
  gsize length = 0;
  GError *err = NULL;
  if (!g_file_get_contents(path, &contents, &length, &err)) {
    fprintf(stderr, "Failed to read %s: %s\n", path, err->message);
    g_error_free(err);
    return false;
  }
  *out_data = (uint8_t *)contents;
  *out_len = length;
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <input.jpg>\n", argv[0]);
    return 1;
  }

  const char *jpeg_path = argv[1];

  /* Read JPEG file */
  g_autofree uint8_t *jpeg_data = NULL;
  size_t jpeg_len = 0;
  if (!read_file(jpeg_path, &jpeg_data, &jpeg_len)) {
    fail("Failed to read JPEG file");
  }
  printf("Read JPEG file: %s (%zu bytes)\n", jpeg_path, jpeg_len);

  /* Get JPEG dimensions first */
  int32_t w = 0, h = 0;
  GError *dim_err = NULL;
  if (!_openslide_jpeg_decode_buffer_dimensions(jpeg_data, (uint32_t) jpeg_len,
                                                &w, &h, &dim_err)) {
    fail("Failed to get JPEG dimensions: %s",
         dim_err ? dim_err->message : "unknown error");
  }
  printf("JPEG dimensions: %dx%d\n", w, h);

  /* Allocate and decode JPEG */
  g_autofree uint32_t *jpeg_pixels = g_new(uint32_t, (size_t) w * h);
  GError *jpeg_err = NULL;
  if (!_openslide_jpeg_decode_buffer(jpeg_data, (uint32_t) jpeg_len,
                                     jpeg_pixels, w, h, &jpeg_err)) {
    fail("Failed to decode JPEG: %s",
         jpeg_err ? jpeg_err->message : "unknown error");
  }
  printf("Decoded JPEG: %dx%d\n", w, h);

  /* Create temporary JXL file */
  g_autofree char *jxl_path = NULL;
  GError *err = NULL;
  int fd = g_file_open_tmp("test_XXXXXX.jxl", &jxl_path, &err);
  if (fd == -1) {
    fail("Failed to create temp file: %s", err->message);
  }
  close(fd);
  printf("Temp JXL file: %s\n", jxl_path);

  /* Convert JPEG to JXL using cjxl */
  g_autofree char *cjxl_cmd = g_strdup_printf(
      "cjxl '%s' '%s' --lossless_jpeg=1 -q 100 2>&1",
      jpeg_path, jxl_path);
  printf("Running: %s\n", cjxl_cmd);

  int ret = system(cjxl_cmd);
  if (ret != 0) {
    unlink(jxl_path);
    fail("cjxl failed with exit code %d", ret);
  }
  printf("Converted JPEG to JXL\n");

  /* Read JXL file */
  g_autofree uint8_t *jxl_data = NULL;
  size_t jxl_len = 0;
  if (!read_file(jxl_path, &jxl_data, &jxl_len)) {
    unlink(jxl_path);
    fail("Failed to read JXL file");
  }
  printf("Read JXL file: %zu bytes\n", jxl_len);

  /* Decode JXL - dimensions should match JPEG since it's lossless transcode */
  g_autofree uint32_t *jxl_pixels = g_new(uint32_t, (size_t) w * h);
  GError *jxl_err = NULL;
  if (!_openslide_jpegxl_decode_buffer(jxl_pixels,
                                       w, h,
                                       jxl_data, (int32_t) jxl_len,
                                       &jxl_err)) {
    unlink(jxl_path);
    fail("Failed to decode JXL: %s",
         jxl_err ? jxl_err->message : "unknown error");
  }
  printf("Decoded JXL: %dx%d\n", w, h);

  /* Clean up temp file */
  unlink(jxl_path);

  /* Compare pixels */
  size_t pixel_count = (size_t)w * (size_t)h;
  int diff_count = 0;
  int first_diff_idx = -1;
  for (size_t i = 0; i < pixel_count; i++) {
    if (jpeg_pixels[i] != jxl_pixels[i]) {
      if (first_diff_idx == -1) {
        first_diff_idx = (int)i;
      }
      diff_count++;
    }
  }

  /* Report results */
  if (diff_count == 0) {
    printf("\n*** SUCCESS: All %zu pixels are identical! ***\n", pixel_count);
    return 0;
  } else {
    printf("\n*** FAILURE: %d pixels differ out of %zu (%.2f%%) ***\n",
           diff_count, pixel_count,
           100.0 * diff_count / pixel_count);

    /* Show first difference details */
    int x = first_diff_idx % w;
    int y = first_diff_idx / w;
    uint32_t jp = jpeg_pixels[first_diff_idx];
    uint32_t jxlp = jxl_pixels[first_diff_idx];
    printf("First difference at pixel %d (%d, %d):\n", first_diff_idx, x, y);
    printf("  JPEG:    0x%08x (R=%d G=%d B=%d A=%d)\n",
           jp, (jp >> 16) & 0xff, (jp >> 8) & 0xff, jp & 0xff, (jp >> 24) & 0xff);
    printf("  JXL:     0x%08x (R=%d G=%d B=%d A=%d)\n",
           jxlp, (jxlp >> 16) & 0xff, (jxlp >> 8) & 0xff, jxlp & 0xff, (jxlp >> 24) & 0xff);

    /* Compute max channel difference */
    int max_diff = 0;
    for (size_t i = 0; i < pixel_count; i++) {
      uint32_t p1 = jpeg_pixels[i];
      uint32_t p2 = jxl_pixels[i];
      for (int c = 0; c < 4; c++) {
        int v1 = (p1 >> (c * 8)) & 0xff;
        int v2 = (p2 >> (c * 8)) & 0xff;
        int d = abs(v1 - v2);
        if (d > max_diff) max_diff = d;
      }
    }
    printf("Max channel difference: %d\n", max_diff);

    return 1;
  }
}
