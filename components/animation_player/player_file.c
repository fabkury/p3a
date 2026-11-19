// SPDX-License-Identifier: Apache-2.0
// Reads a whole artwork file into memory.

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_err.h"
#include "p3a_limits.h"
#include "player_file.h"

static const char *TAG = "player";

#define READ_CHUNK_BYTES (64 * 1024)
#define READ_RETRIES     3
#define READ_RETRY_MS    50

// The ESP32-P4's SD host transfers straight into a PSRAM buffer only when
// both its address and its size are multiples of the cache line. Otherwise
// every 512-byte sector goes through a bounce buffer with an SD command of
// its own: about 1000 commands per 500 KB (docs/jitter/REPORT.md, fix 1).
#define SD_DMA_ALIGN 128

esp_err_t player_file_read(const char *path, uint8_t **data_out, size_t *size_out)
{
    // POSIX read(), not fread() (jitter fix 6). newlib's stdio hands FATFS one
    // 512-byte sector per SD command whatever the fread() size: a 4.6 MB file
    // was 9523 single-sector reads over 5.6 s, and such command storms
    // stalled playback. read() passes the whole 64 KB request to FATFS, which
    // reads whole clusters into the aligned buffer.
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        const int why = errno;
        if (why == ENOENT || why == ENOTDIR) {
            return P3A_ERR_MISSING;
        }
        ESP_LOGW(TAG, "cannot open %s (errno %d)", path, why);
        return ESP_FAIL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        ESP_LOGW(TAG, "cannot stat %s (errno %d)", path, errno);
        close(fd);
        return ESP_FAIL;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return P3A_ERR_NOT_PLAYABLE;
    }
    if (st.st_size <= 0) {
        close(fd);
        return P3A_ERR_CORRUPT;  // an empty file is a download that never arrived
    }
    if (st.st_size > P3A_MAX_ARTWORK_BYTES) {
        ESP_LOGW(TAG, "%s is %ld bytes, over the %d limit", path, (long)st.st_size, P3A_MAX_ARTWORK_BYTES);
        close(fd);
        return P3A_ERR_NOT_PLAYABLE;
    }
    const size_t size = (size_t)st.st_size;
    const size_t alloc = (size + SD_DMA_ALIGN - 1) & ~(size_t)(SD_DMA_ALIGN - 1);
    // Aligned, so not through psram_alloc.h, which has no aligned form.
    uint8_t *buffer = heap_caps_aligned_alloc(SD_DMA_ALIGN, alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }

    size_t have = 0;
    int retries = 0;
    while (have < size) {
        const size_t want = (size - have < READ_CHUNK_BYTES) ? size - have : READ_CHUNK_BYTES;
        ssize_t got = read(fd, buffer + have, want);
        if (got < 0 && retries < READ_RETRIES) {
            retries++;
            ESP_LOGW(TAG, "read error at %u of %s (errno %d), retry %d", (unsigned)have, path, errno, retries);
            vTaskDelay(pdMS_TO_TICKS(READ_RETRY_MS * retries));
            continue;
        }
        if (got <= 0) {
            // Shorter than its directory entry says, or unreadable. Either way
            // the card answered wrongly; the bytes read so far prove nothing
            // about the file.
            ESP_LOGE(TAG, "read failed at %u of %u in %s", (unsigned)have, (unsigned)size, path);
            close(fd);
            free(buffer);
            return ESP_FAIL;
        }
        have += (size_t)got;
        retries = 0;
        taskYIELD();
    }
    close(fd);
    *data_out = buffer;
    *size_out = size;
    return ESP_OK;
}
