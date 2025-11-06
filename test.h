#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *filename;
    const uint8_t *data;
    size_t size;
} embedded_file_t;

extern const uint8_t _binary__________MARK_5_EARLIER_COMMIT_main_decoded_3826495_webp_start[] asm("_binary__________MARK_5_EARLIER_COMMIT_main_decoded_3826495_webp_start");
extern const uint8_t _binary__________MARK_5_EARLIER_COMMIT_main_decoded_3826495_webp_end[] asm("_binary__________MARK_5_EARLIER_COMMIT_main_decoded_3826495_webp_end");

#define embedded_files_count 1

static const embedded_file_t embedded_files[] = {
    {"decoded_3826495.webp", _binary__________MARK_5_EARLIER_COMMIT_main_decoded_3826495_webp_start, (size_t)(_binary__________MARK_5_EARLIER_COMMIT_main_decoded_3826495_webp_end - _binary__________MARK_5_EARLIER_COMMIT_main_decoded_3826495_webp_start)},
};
