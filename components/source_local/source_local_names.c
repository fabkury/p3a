// SPDX-License-Identifier: Apache-2.0
// Which files in the user's folder are pictures. Pure.
#include <string.h>
#include <strings.h>

#include "source_local.h"

p3a_format_t source_local_format_of(const char *filename)
{
    static const struct {
        const char *ext;
        p3a_format_t format;
    } known[] = {
        {".webp", P3A_FORMAT_WEBP}, {".gif", P3A_FORMAT_GIF},   {".png", P3A_FORMAT_PNG}, {".apng", P3A_FORMAT_PNG},
        {".jpg", P3A_FORMAT_JPEG},  {".jpeg", P3A_FORMAT_JPEG}, {".bmp", P3A_FORMAT_BMP},
    };
    // Hidden files, and the "._name" companions macOS leaves on FAT cards,
    // which carry a picture's extension and are not pictures.
    if (filename == NULL || filename[0] == '.' || filename[0] == '\0') {
        return P3A_FORMAT_UNKNOWN;
    }
    const char *dot = strrchr(filename, '.');
    if (dot == NULL || dot == filename) {
        return P3A_FORMAT_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcasecmp(dot, known[i].ext) == 0) {  // FAT does not keep case reliably
            return known[i].format;
        }
    }
    return P3A_FORMAT_UNKNOWN;
}
