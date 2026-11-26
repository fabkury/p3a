# The HTTP API

Everything the device's web pages do goes through this API, and it is open to anything on the same network. There is no authentication: the API is for the LAN. It is not reachable from the Internet unless you make it so, and you should not.

The source of truth is the route tables in the code (`components/p3a_net/net_*_api.c`, `components/p3a_update/update_api.c`); each file's header lists its routes.

## The shape of every answer

```json
{"ok": true, "data": { ... }}
{"ok": false, "code": "NOTHING_TO_PLAY", "error": "there is nothing earlier in the history"}
```

`code` is for programs and does not change; `error` is a sentence for people. The HTTP status says the kind of failure (400 a bad request, 404 no such thing, 409 not now, 5xx the device's own trouble).

Work that takes longer than a moment is a **job**: the answer is `202` with `{"job": <number>}`, and `GET /api/v2/jobs/<number>` gives its `state`, `progress` (0 to 100) and, once done, `succeeded` and an `error` code if it failed.

A `GET` carries an `ETag`; send it back as `If-None-Match` and an unchanged answer costs a few bytes (`304`).

## The device

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/status` | Version, network, time, storage, memory, the chip, the co-processor, whether a crash record exists |
| GET | `/api/v2/jobs/{id}` | A job's state and progress |
| POST | `/api/v2/system/reboot` | Restart |
| POST | `/api/v2/system/factory-reset` | Body `{"confirm": "factory-reset"}`. Erases settings, Wi-Fi and registrations, then restarts. The SD card is kept. A job |
| GET | `/api/v2/system/coredump` | The last crash record, raw. It is a copy of memory and may hold secrets |
| DELETE | `/api/v2/system/coredump` | Erase it |
| GET | `/api/v2/ui` | The screen on top, the notice if one is seen, and their words |
| GET | `/api/v2/ui/intro-animations` | The boot animations' names |

## Settings

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/settings/schema` | Every setting with its type, limits, default and label. Served as written, without the envelope |
| GET | `/api/v2/settings` | Every setting's value. API keys read as `********` when set and `""` when not |
| PATCH | `/api/v2/settings` | Change any number of settings, all or nothing. Answers `changed` and `reboot_needed`. Sending `********` for a key keeps it; `""` removes it |
| GET | `/api/v2/secrets/museum-keys` | The Harvard and Smithsonian keys, unmasked: the museum browser runs in the browser and must send them |

## Wi-Fi

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/wifi` | The stored network's name, the state, whether the device is in setup |
| PUT | `/api/v2/wifi` | `{"ssid", "password", "device_name"}`, the last two optional. Stores them and restarts |
| DELETE | `/api/v2/wifi` | Forget the network and restart into setup |

## Playback

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/player` | What is playing (with a `file` address for a preview), the active playset's channels and their state, the history, counters |
| POST | `/api/v2/player/next`, `/back` | The next artwork; the one before |
| POST | `/api/v2/player/pause`, `/resume` | Black panel, backlight off, no moving on; and back |
| POST | `/api/v2/player/refresh` | List every channel of the active playset again, now |
| POST | `/api/v2/player/like`, `/unlike` | What the artwork's source makes of a like: a thumbs-up on Makapix Club, a click on Giphy |
| POST | `/api/v2/player/show` | `{"url"}`: fetch the picture at that address, keep it among the user's files and play it. A job |
| GET | `/api/v2/player/playset` | The active playset |
| PUT | `/api/v2/player/playset` | Make a playset active: a whole playset, or `{"saved": "<name>"}` |
| POST | `/api/v2/files?name=<file>` | Upload a picture into the user's folder; the body is the file. It plays at once |
| GET | `/api/v2/blobs/{reference}.{ext}` | A downloaded artwork's bytes. Never changes, so it is cached for good |
| GET | `/api/v2/files/{name}` | One of the user's own files |

A playset is `{"name", "channels": [{"source", "kind", "identifier", "display_name", "weight", "offset"}]}`, at most 64 channels. `source` is one of `local`, `makapix`, `giphy`, `klipy`, `museum`, `pinned`; what `kind` and `identifier` mean is each source's (`playset.h` and the source's header).

## Saved playsets

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/playsets` | Names and channel counts, at most 32 |
| GET | `/api/v2/playsets/{name}` | One playset |
| PUT | `/api/v2/playsets/{name}` | Save one, new or replacing |
| DELETE | `/api/v2/playsets/{name}` | Delete one |

## Pin lists

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/pins` | The lists: slug, name, whether active, how many pins |
| POST | `/api/v2/pins` | `{"name"}`: a new list |
| GET | `/api/v2/pins/{slug}` | A list's pins, a page at a time (`?offset=`, `?limit=` up to 64) |
| PUT | `/api/v2/pins/{slug}` | `{"name"}` renames it, `{"active": true}` makes it the active list |
| DELETE | `/api/v2/pins/{slug}` | The list and its pins. Not the active list |
| POST | `/api/v2/pins/{slug}/pin` | Pin the artwork on the screen |
| POST | `/api/v2/pins/{slug}/unpin` | `{}` unpins the artwork on the screen; `{"position", "key"}` unpins a listed one |

`active` stands for the active list's slug.

## Sources

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/sources/museums` | The museums this firmware plays: id, name, axes, whether one needs a key and has it, and why one cannot be played now |
| POST | `/api/v2/sources/giphy/reset-id` | Forget the anonymous id Giphy gave this device |
| GET | `/api/v2/makapix` | Whether the device is registered, a registration under way (its code and seconds left), the link's state |
| POST | `/api/v2/makapix/registration` | Start registering: the device asks Makapix Club for a code and shows it |
| DELETE | `/api/v2/makapix/registration` | Cancel a registration under way; with none under way, remove the registration |

## Firmware updates

| Method | Path | What |
|---|---|---|
| GET | `/api/v2/update` | What runs, whether it is on trial, the firmware in the other slot, the newest release known, what is going on |
| POST | `/api/v2/update/check` | Look for releases now. A job |
| POST | `/api/v2/update/install` | Install the newer release that was found. A job; the device restarts when it ends well |
| POST | `/api/v2/update/rollback` | Go back to the firmware in the other slot. A job; restarts |

## Not in a release

Builds made for testing add `/api/v2/test/...` (the test hooks) and builds that trace add `/api/v2/debug/frames...` (the frame trace). `tools/make_release.py` refuses to package either.
