// SPDX-License-Identifier: Apache-2.0
// A channel's file as playsets of different sizes share it: see p3a_index.h.
// Pure: host-tested (tests/host/library/test_index_shared.c).
#include <string.h>

#include "p3a_index.h"

void p3a_index_shared_read(p3a_record_t *records, uint32_t count, p3a_index_listing_t *out)
{
    out->listed = count;
    out->cut = false;
    for (uint32_t i = 0; i < count; i++) {
        if ((records[i].flags & P3A_RECORD_LISTING_END) == 0) {
            continue;
        }
        records[i].flags &= (uint8_t)~P3A_RECORD_LISTING_END;
        if (!out->cut) {  // a file has one; of several, the first is believed
            out->listed = i + 1;
            out->cut = true;
        }
    }
}

uint32_t p3a_index_shared_pack(const p3a_index_t *mine, const p3a_index_listing_t *listing, const p3a_record_t *old,
                               uint32_t old_count, const p3a_index_listing_t *old_listing, p3a_record_t *out)
{
    uint32_t n = mine->count < P3A_INDEX_MAX_RECORDS ? mine->count : P3A_INDEX_MAX_RECORDS;
    if (n > 0) {
        memcpy(out, mine->records, (size_t)n * sizeof(p3a_record_t));
    }
    uint32_t head = n;
    const bool start_of_longer = listing->listed > mine->count;
    uint32_t room = old_count > n ? old_count : n;
    if (room > P3A_INDEX_MAX_RECORDS) {
        room = P3A_INDEX_MAX_RECORDS;
    }
    for (uint32_t i = 0; old != NULL && i < old_count && n < room; i++) {
        const bool of_head = start_of_longer && old_listing != NULL && i < old_listing->listed;
        if (!of_head && !(listing->cut && p3a_record_has_blob(&old[i]))) {
            continue;
        }
        if (p3a_index_find_record(mine, &old[i]) >= 0) {
            continue;
        }
        out[n++] = old[i];
        if (of_head) {
            head = n;
        }
    }
    if (listing->cut && head > 0) {
        out[head - 1].flags |= P3A_RECORD_LISTING_END;
    }
    return n;
}
