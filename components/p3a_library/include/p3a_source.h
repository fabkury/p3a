// SPDX-License-Identifier: Apache-2.0
// The source interface: what every place artworks come from must be able to
// do, as a table of functions. A source registers its table at boot; the
// scheduler, its content task and the API call through it and never name
// a source.
//
// 1.x had no such table: each source was an arm in about a dozen type
// ladders (63 comparisons in the refresh code alone), and each new source
// added an arm to every one.
//
// The interface lives in the library layer and the sources sit above it:
// they include this header and the record's, and nothing below includes
// theirs. Pure (host-tested).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_index.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

// A channel's source number: p3a_source_t for the five places artworks come
// from, and one more for pinned lists, whose records keep their own source.
// The same numbers as playset_source_t; written to disk.
#define P3A_CHANNEL_SOURCE_PINNED 6
#define P3A_CHANNEL_SOURCE_TEST   7  // served by the test hooks; no such source in a release build
#define P3A_CHANNEL_SOURCE_END_   8

// Which of a source's channels: the three fields of a playset channel that,
// with the source, make its id.
typedef struct {
    const char *kind;
    const char *identifier;
    uint32_t offset;
    // Whether the listing is still wanted. A source whose listing takes
    // several requests asks between them (and hands it to its requests as
    // their `should_abort`), and answers ESP_ERR_NOT_FINISHED when it is
    // not: the playset was changed, and its channels should not wait
    // behind a listing nobody will use. NULL: never stopped.
    bool (*stop)(void *ctx);
    void *stop_ctx;
} p3a_channel_ref_t;

// For a source's listing loop: true when the listing should end now.
static inline bool p3a_channel_stopped(const p3a_channel_ref_t *channel)
{
    return channel->stop != NULL && channel->stop(channel->stop_ctx);
}

// What a source made of a person's swipe on one of its artworks.
typedef enum {
    P3A_REACTION_NONE = 0,  // nothing: the source has no such thing, or cannot now
    P3A_REACTION_LIKED,
    P3A_REACTION_UNLIKED,
    P3A_REACTION_NOTED,     // told to the source, which keeps no state of it (Giphy's click)
} p3a_reaction_t;

// How one of a source's artworks came to be on the panel.
typedef struct {
    uint8_t channel_source;  // the source of the channel it was picked from (6 for a pin list); 0 when it
                             // was not picked from a channel (asked for by itself, or replayed from history)
    const char *kind;        // that channel's kind and identifier; "" when there is none
    const char *identifier;
    bool asked_for;          // a person asked for this artwork by itself
    bool random;             // the order channels are played in: random, or the channel's own
} p3a_shown_t;

// What one channel of a playset may ask for itself in place of its source's
// two settings below (playset.h), each end included.
typedef struct {
    uint32_t capacity_min, capacity_max;      // records
    uint32_t refresh_min_s, refresh_max_s;    // seconds
} p3a_channel_range_t;

typedef struct {
    uint8_t source;    // the channel source number this table serves
    const char *name;  // "local", "giphy", ...

    // Lists the channel into `index`, which is empty: what the channel holds
    // now, in its order. The caller brings over what the device already
    // knew about those artworks (p3a_index_carry_over). May take long (a
    // card scan, network requests): it runs on the content task. P3A_ERR_NO_KEY, P3A_ERR_RATE_LIMITED,
    // P3A_ERR_AUTH and P3A_ERR_SD_FAILED mean what p3a_err.h says.
    esp_err_t (*refresh)(const p3a_channel_ref_t *channel, p3a_index_t *index);

    // True for a source whose channels are listed over the network and
    // whose artworks are downloaded into the blob store. Its channels are
    // not listed without a network, and their indices are kept on the card.
    bool remote;
    // Where to download a record's bytes from. May ask the network itself
    // (a source that must look the address up); P3A_ERR_MISSING when the
    // artwork no longer exists. NULL for a source that is not remote.
    esp_err_t (*url)(const p3a_record_t *record, char *out, size_t len);
    // The source's mark, drawn in the band under its artworks when there
    // is one ("Powered by GIPHY", as the service drew it): a picture file
    // made by tools/gen_image.py --soft and compiled into the source's
    // component, from its first byte to the one after its last. NULL for
    // none. `credit` is the mark for a dark background colour;
    // `credit_light` the one for a light background, or NULL when the
    // first serves for both.
    const uint8_t *credit, *credit_end;
    const uint8_t *credit_light, *credit_light_end;
    // What its downloads say they are; NULL for "p3a".
    const char *user_agent;
    // A download that is refused (401, 403) says something about that
    // artwork, not about a key: a museum's image server refusing one
    // picture. It counts against the artwork and the source carries on.
    bool refusal_is_the_artworks;
    // Its downloads may be sent on to another address (up to five times).
    // Followed by hand, as 1.x did for the museums' image servers.
    bool redirects;
    // A person swiped up on this artwork while it played (down, with
    // `undo`). Answers at once: anything that takes time is the source's
    // own job, and its outcome is not waited for. NULL for a source with
    // nothing to tell.
    p3a_reaction_t (*react)(const p3a_record_t *record, bool undo);
    // One of this source's artworks is now seen on the panel (`record` and
    // `how`, both valid only during the call), or none of them is any more
    // (`record` NULL): another artwork took its place, a screen covers it,
    // or the player is paused. Told once per change, the same artwork again
    // after a cover included. Called from whichever task made the change:
    // it must return at once. NULL for a source that does not care.
    void (*shown)(const p3a_record_t *record, const p3a_shown_t *how);
    // Where a record's bytes are when they are not in the blob store (records
    // flagged P3A_RECORD_IN_PLACE). NULL for a source whose artworks are
    // always blobs.
    esp_err_t (*path)(const p3a_record_t *record, char *out, size_t len);

    // How often a channel should be listed again, in seconds; 0 for a source
    // that is listed only when something says it changed.
    uint32_t (*refresh_interval_s)(void);

    // How many records one of its channels may hold (a setting, for most
    // sources). NULL for 1024.
    uint32_t (*index_capacity)(void);

    // The ranges within which one channel of a playset may set the two
    // values above for itself: those of the source's own settings. NULL for
    // a source whose channels take no value of their own (the card, a pin
    // list).
    void (*channel_range)(p3a_channel_range_t *out);

    // Whether this source's pictures are scaled smoothly (photographs,
    // paintings) or with sharp pixels (pixel art).
    bool smooth;

    // Whether very elongated artworks of its channels are passed over (the
    // `aspect_filter` settings): for sources whose pictures come in any
    // shape and were not chosen one by one by the user.
    bool filter_aspect;
} p3a_source_ops_t;

// Registers a source. The table must stay valid for good (make it static).
// ESP_ERR_INVALID_ARG for a missing name or refresh function or a number out
// of range; ESP_ERR_INVALID_STATE when that number is already taken.
esp_err_t p3a_source_register(const p3a_source_ops_t *ops);

// The table of a channel source, or NULL when none is registered: a playset
// may name a source this build does not have, and its channel is then
// simply never chosen.
const p3a_source_ops_t *p3a_source_get(uint8_t source);

// For tests: forgets every registration.
void p3a_source_reset_for_test(void);

#ifdef __cplusplus
}
#endif
