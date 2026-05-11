// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file idf_jpeg_release_null_fix.c
 * @brief Linker --wrap shim that NULL-checks jpeg_release_codec_handle()
 *
 * ESP-IDF v5.5.2's components/esp_driver_jpeg/jpeg_common.c has a bug in
 * jpeg_release_codec_handle(): line 88 gates the cleanup on the GLOBAL
 *
 *   if (s_jpeg_platform.jpeg_codec) {
 *       ...
 *       if (jpeg_codec->codec_mutex) { ... }   <- NULL deref if param is 0
 *   }
 *
 * but then dereferences the PARAMETER. When jpeg_new_decoder_engine fails
 * partway through (e.g. dma2d_acquire_pool returns ESP_ERR_NOT_FOUND on
 * pool exhaustion, observed in the field with concurrent TLS handshakes
 * pressuring internal RAM), its err: cleanup calls
 * jpeg_del_decoder_engine(), which then calls
 * jpeg_release_codec_handle(decoder_engine->codec_base) — and codec_base
 * is still NULL because the codec wasn't allocated yet. The global is
 * non-NULL because earlier decodes succeeded, so the guard passes and
 * line 94 (jpeg_codec->codec_mutex) loads from address 0 → Guru
 * Meditation, Load access fault.
 *
 * Fix is one line: return ESP_OK immediately on NULL input, matching the
 * shape of free(NULL) and most well-behaved release helpers. We wire it
 * in via `-Wl,--wrap=jpeg_release_codec_handle` so all internal callers
 * (jpeg_del_decoder_engine in jpeg_decode.c, the equivalent encoder
 * path in jpeg_encode.c) get the safe behaviour without forking the IDF
 * component. The same wrap-and-force-undefined pattern p3a already uses
 * for ff_memalloc / ff_memfree (main/CMakeLists.txt).
 *
 * Remove this file when ESP-IDF fixes jpeg_release_codec_handle upstream.
 */

#include "esp_err.h"

// jpeg_codec_handle_t is a typedef for a pointer to an IDF-internal
// struct (jpeg_codec_t); using void* lets us avoid pulling the private
// header. The calling convention for a single pointer parameter is the
// same either way on RV32, and we don't dereference the pointer here.
esp_err_t __real_jpeg_release_codec_handle(void *jpeg_codec);

esp_err_t __wrap_jpeg_release_codec_handle(void *jpeg_codec)
{
    if (!jpeg_codec) {
        // IDF v5.5.2 bug guard — see file header.
        return ESP_OK;
    }
    return __real_jpeg_release_codec_handle(jpeg_codec);
}
