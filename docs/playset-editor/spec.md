# Playset Editor — Specification for Web Designer

## Context

This is a new page for the p3a web UI — an ESP32-based pixel art player. The page lets users create, edit, and manage **playsets** (playback configurations that define which artwork channels to play and how to balance them). The page will be added at `/playset-editor.html`.

## Existing Design System (must match)

- **No frameworks** — vanilla HTML5/CSS3/JavaScript only (ES5 compatible)
- **Font**: `-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif`, weight 300 for headers
- **Primary gradient**: `linear-gradient(135deg, #667eea 0%, #764ba2 100%)`  (purple)
- **Cards**: white background, `border-radius: 16px`, subtle shadow
- **Buttons**: `border-radius: 8-12px`, primary = blue gradient, danger = red
- **Toggle switches**: checkbox styled as slider with pseudo-element
- **Layout**: centered flex column, `max-width: min(520px, 100%)`, responsive with `clamp()` sizing
- **Toast notifications**: fixed top-center, auto-hide after 5-6 seconds
- **Responsive**: mobile-first, breakpoints at 480px
- **API calls**: `XMLHttpRequest` or `fetch`, all responses are `{"ok": true/false, "data": {...}, "error": "..."}`

## What a Playset Contains

A playset is a named playback configuration with these properties:

| Property | Type | Description |
|----------|------|-------------|
| **name** | string, 1-32 chars | Unique identifier for the playset (alphanumeric + underscore) |
| **exposure_mode** | enum | How to balance play time across channels |
| **pick_mode** | enum | How to select artwork within each channel |
| **channels** | array, 1-64 items | List of content sources to include |

## Exposure Modes (choose one)

| Value | Label | Description |
|-------|-------|-------------|
| `equal` | Equal | All channels get the same play time. Weights are ignored. |
| `manual` | Manual | Each channel has an explicit weight controlling how often it plays relative to others. |
| `proportional` | Proportional | Channels are automatically weighted by how much recent content they have. Weights are ignored. |

## Pick Modes (choose one)

| Value | Label | Description |
|-------|-------|-------------|
| `recency` | Newest First | Plays artwork from newest to oldest in deterministic order. |
| `random` | Random | Randomly selects artwork within each channel. |

## Channel Types

Each channel in the playset is one of these types:

| Type | Label | Required Fields | Description |
|------|-------|----------------|-------------|
| `named` | Named Channel | `name`: must be `"all"` or `"promoted"` | Built-in Makapix channels. "all" = all recent artworks, "promoted" = curated/promoted artworks. |
| `user` | Artist | `identifier`: artist's sqid (short alphanumeric ID) | Artworks by a specific Makapix artist. |
| `hashtag` | Hashtag | `identifier`: tag name (no # prefix) | Artworks tagged with a specific hashtag on Makapix. |
| `sdcard` | SD Card | (none) | Local files stored on the device's SD card. |
| `giphy` | Giphy | `name`: `"trending"` or `"search_{query}"` | GIFs from Giphy. "trending" = trending GIFs. "search_{query}" = search results for a query. |

Each channel also has:
- **display_name** (string, 0-64 chars, optional): human-readable label shown in the UI
- **weight** (integer, 0-4294967295): only meaningful when exposure_mode is `manual`. 0 = auto-calculate. Higher = more play time relative to others.

## Page Functionality

### 1. Playset List (Landing State)

- Show a list of saved playsets fetched from `GET /playsets` (endpoint to be created — returns `{"ok": true, "data": {"playsets": [{"name": "my_mix", "channel_count": 3, "exposure_mode": "manual", "pick_mode": "random"}, ...]}}`)
- Each item shows: name, channel count, exposure mode, pick mode
- Each item has: **Play** button (`POST /playset/{name}`), **Edit** button, **Delete** button (`DELETE /playset/{name}`)
- **"Create New Playset"** button at top

### 2. Playset Editor Form

- **Playset Name**: text input, 1-32 characters, alphanumeric and underscores only. Editable only when creating new (read-only when editing existing).
- **Exposure Mode**: radio buttons or segmented toggle for `equal` / `manual` / `proportional`. Show a short description of the selected mode below the selector.
- **Pick Mode**: radio buttons or segmented toggle for `recency` / `random`. Show a short description of the selected mode below the selector.

### 3. Channel List (within editor)

- Displays the channels currently in this playset as an ordered list of cards
- Each channel card shows:
  - Channel type icon/badge (e.g., "Named", "Artist", "Hashtag", "SD Card", "Giphy")
  - Display name (editable text field)
  - Type-specific fields (see below)
  - Weight field — **only visible when exposure_mode is `manual`**. Number input.
  - **Remove** button (trash icon)
- Channels can be **reordered** via drag-and-drop or up/down buttons
- Maximum 64 channels, minimum 1 (cannot remove the last channel)

### 4. Add Channel

- **"Add Channel"** button below the channel list
- Opens a selector/modal to choose the channel type
- Based on the chosen type, shows the appropriate input fields:
  - **Named Channel**: dropdown with options "All Recent Artworks" (`all`) and "Promoted Artworks" (`promoted`)
  - **Artist (User)**: text input for the artist's sqid (short ID). Optional display name field.
  - **Hashtag**: text input for the tag name (no # prefix). Optional display name field.
  - **SD Card**: no additional fields needed (only one SD card channel makes sense, warn if already added)
  - **Giphy**: radio choice between "Trending" and "Search". If "Search", text input for search query. Optional display name field.
- **Confirm** adds the channel to the list

### 5. Weight Visualization

- When exposure_mode is `manual`, show a visual proportional bar/chart next to or above the channel list that illustrates the relative weights. This helps users understand the proportions intuitively.
- Update the visualization in real-time as weights are edited.

### 6. Save / Cancel / Play

- **Save** button: sends the playset to `PUT /playset/{name}` with JSON body:
  ```json
  {
    "exposure_mode": "manual",
    "pick_mode": "random",
    "channels": [
      {
        "type": "named",
        "name": "all",
        "identifier": "",
        "display_name": "Recent Artworks",
        "weight": 100
      },
      {
        "type": "user",
        "name": "user",
        "identifier": "bob",
        "display_name": "Bob's Art",
        "weight": 200
      }
    ]
  }
  ```
- **Save & Play** button: saves, then immediately executes via `POST /playset/{name}`
- **Cancel** button: returns to the playset list without saving
- Show toast notification on success/error

### 7. Validation Rules

- Name: 1-32 chars, `[a-zA-Z0-9_]` only
- At least 1 channel, at most 64
- Channel name fields: max 32 chars
- Channel identifier: max 32 chars
- Display name: max 64 chars
- Weight: non-negative integer (0 = auto-calculate)
- Named channel name must be `"all"` or `"promoted"`
- Giphy channel name must be `"trending"` or start with `"search_"`
- Show inline validation errors, disable Save when invalid

## API Endpoints

These are the endpoints the page communicates with:

| Method | Endpoint | Purpose |
|--------|----------|---------|
| `GET` | `/playsets` | List all saved playsets (name, channel_count, exposure_mode, pick_mode) |
| `GET` | `/playset/{name}` | Get full playset definition for editing |
| `PUT` | `/playset/{name}` | Save/update a playset (JSON body as shown above) |
| `DELETE` | `/playset/{name}` | Delete a saved playset |
| `POST` | `/playset/{name}` | Execute/play a playset (already exists) |

**Note**: `GET /playsets`, `GET /playset/{name}`, `PUT /playset/{name}`, and `DELETE /playset/{name}` are new endpoints that will be implemented on the firmware side. The page should be designed assuming they exist.

## Navigation

- Add a link/button on the main page (`index.html`) that navigates to `/playset-editor.html`
- The playset editor page should have a back/home link to return to `index.html`

## UX Considerations

- The primary use case is creating a mix of several channels (e.g., "show me 50% artwork from artist X, 30% from hashtag Y, and 20% trending Giphy GIFs")
- Users may not be familiar with the terminology — use plain-language labels and short descriptions for each option
- The form should feel like assembling a playlist/mix, not filling out a technical form
- Mobile-friendly is essential — the device is often controlled from a phone
- Keep the page lightweight — it will be stored in a 4MB flash partition alongside other UI pages
