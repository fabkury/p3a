# Makapix Club - Physical Player Integration Guide

This document describes how physical player devices (like p3a) should interact with the Makapix Club API and MQTT broker.

## Overview

Physical players are devices that display pixel art from Makapix Club. They:
1. **Provision** themselves with the API to get a registration code
2. Display the registration code on their screen for the owner to enter on the website
3. **Connect to MQTT** to receive commands and report status
4. Execute commands (swap artwork, show specific artwork)
5. Report their status periodically

---

## 1. Player Provisioning (Registration Flow)

### Step 1: Device Calls Provision Endpoint

When the player boots up for the first time (or needs re-registration), it should call:

```
POST https://makapix.club/api/player/provision
Content-Type: application/json

{
  "device_model": "p3a",
  "firmware_version": "1.0.0"
}
```

Both fields are optional but recommended for tracking.

### Step 2: API Returns Registration Details

**Response (201 Created):**

```json
{
  "player_key": "550e8400-e29b-41d4-a716-446655440000",
  "registration_code": "A3F8X2",
  "registration_code_expires_at": "2025-01-29T12:15:00Z",
  "mqtt_broker": {
    "host": "makapix.club",
    "port": 8883
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `player_key` | UUID | Unique identifier for this player. **Store this permanently.** Used as MQTT username. |
| `registration_code` | String (6 chars) | Display this to the user. Uppercase alphanumeric, excludes ambiguous chars (0, O, I, 1, L). |
| `registration_code_expires_at` | ISO 8601 datetime | Code expires in **15 minutes**. |
| `mqtt_broker.host` | String | MQTT broker hostname. |
| `mqtt_broker.port` | Integer | MQTT broker port (TLS). |

### Step 3: Display Registration Code

The player should display the 6-character `registration_code` prominently on its screen so the owner can enter it on the Makapix Club website.

**Example display:**
```
┌─────────────────────┐
│   REGISTER PLAYER   │
│                     │
│      A3F8X2         │
│                     │
│  Enter this code at │
│  makapix.club       │
│                     │
│  Expires in 14:32   │
└─────────────────────┘
```

### Step 4: Wait for Registration Completion

After displaying the code, the player should:
1. Connect to MQTT using the `player_key` as username
2. Subscribe to its command topic
3. Wait for the first command (which indicates successful registration)

Alternatively, the player can poll its status or simply wait for MQTT messages.

---

## 2. MQTT Connection

### Connection Details

| Parameter | Value |
|-----------|-------|
| **Protocol** | MQTT v5 over TLS |
| **Host** | `makapix.club` (or value from provision response) |
| **Port** | `8883` (TLS) |
| **Username** | The `player_key` UUID (e.g., `550e8400-e29b-41d4-a716-446655440000`) |
| **Password** | Empty string (authentication is by username only for now) |
| **Client ID** | Any unique string (e.g., `p3a-{player_key}`) |
| **Keep Alive** | 60 seconds recommended |

### TLS Configuration

- The broker uses TLS on port 8883
- Players **must** verify the server certificate using the Makapix CA certificate
- CA certificate files are in `certs/player/`:
  - `makapix_ca.crt` - PEM format for general use
  - `makapix_ca.inc` - C include format for embedding in firmware

**Important:** Use the CA certificate (root certificate), not the server's end-entity certificate. The CA certificate is used to verify the server's identity during TLS handshake.

### Topics

The player uses two topics based on its `player_key`:

| Topic | Direction | QoS | Description |
|-------|-----------|-----|-------------|
| `makapix/player/{player_key}/command` | Server → Player (Subscribe) | 1 | Receives commands from the owner |
| `makapix/player/{player_key}/status` | Player → Server (Publish) | 1 | Reports player status |

**Example for player_key `550e8400-e29b-41d4-a716-446655440000`:**
- Subscribe to: `makapix/player/550e8400-e29b-41d4-a716-446655440000/command`
- Publish to: `makapix/player/550e8400-e29b-41d4-a716-446655440000/status`

---

## 3. Receiving Commands

The player should subscribe to its command topic immediately after connecting.

### Command Message Format

Commands are JSON messages published by the server:

```json
{
  "command_id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "command_type": "swap_next",
  "payload": {},
  "timestamp": "2025-01-29T12:00:00+00:00"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `command_id` | UUID | Unique identifier for this command (for logging/debugging) |
| `command_type` | String | One of: `swap_next`, `swap_back`, `show_artwork` |
| `payload` | Object | Command-specific data (may be empty) |
| `timestamp` | ISO 8601 | When the command was sent |

### Command Types

#### 1. `swap_next`

Move to the next artwork in the player's internal playlist/queue.

```json
{
  "command_id": "...",
  "command_type": "swap_next",
  "payload": {},
  "timestamp": "..."
}
```

**Action:** Display the next artwork in the player's internal rotation.

#### 2. `swap_back`

Move to the previous artwork in the player's internal playlist/queue.

```json
{
  "command_id": "...",
  "command_type": "swap_back",
  "payload": {},
  "timestamp": "..."
}
```

**Action:** Display the previous artwork in the player's internal rotation.

#### 3. `show_artwork`

Display a specific artwork immediately.

```json
{
  "command_id": "...",
  "command_type": "show_artwork",
  "payload": {
    "post_id": 123,
    "storage_key": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "art_url": "https://makapix.club/api/vault/a1b2c3d4-e5f6-7890-abcd-ef1234567890.png",
    "canvas": "64x64"
  },
  "timestamp": "..."
}
```

| Payload Field | Type | Description |
|---------------|------|-------------|
| `post_id` | Integer | Database ID of the artwork |
| `storage_key` | UUID | Storage identifier |
| `art_url` | String | Full URL to download the artwork image |
| `canvas` | String | Canvas dimensions (e.g., "64x64", "128x128") |

**Action:**
1. Download the image from `art_url`
2. Display it immediately
3. Optionally add to internal rotation
4. Update `current_post_id` in next status report

---

## 4. Reporting Status

The player should periodically publish its status to the server.

### When to Report Status

- **On connect:** Immediately after MQTT connection is established
- **Periodically:** Every 30-60 seconds while online
- **On state change:** When the displayed artwork changes
- **On disconnect:** Send status with `"status": "offline"` if possible (Last Will)

### Status Message Format

Publish to `makapix/player/{player_key}/status`:

```json
{
  "player_key": "550e8400-e29b-41d4-a716-446655440000",
  "status": "online",
  "current_post_id": 123,
  "firmware_version": "1.0.0",
  "timestamp": "2025-01-29T12:00:00Z"
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `player_key` | UUID | **Yes** | The player's unique key |
| `status` | String | **Yes** | `"online"` or `"offline"` |
| `current_post_id` | Integer or null | No | The `post_id` of the currently displayed artwork |
| `firmware_version` | String | No | Current firmware version |
| `timestamp` | ISO 8601 | No | When the status was generated |

### MQTT Last Will and Testament (LWT)

Configure the MQTT client with a Last Will message to automatically report offline status if the connection is lost unexpectedly:

```
Will Topic: makapix/player/{player_key}/status
Will Payload: {"player_key": "{player_key}", "status": "offline"}
Will QoS: 1
Will Retain: false
```

---

## 5. Artwork Management

### Internal Artwork Queue

The player maintains its own internal queue/playlist of artworks. This queue is managed locally on the device:

- When receiving `show_artwork`, add the artwork to the queue and display it
- When receiving `swap_next`/`swap_back`, cycle through the internal queue
- The server does **not** manage the player's queue; it only sends commands

### Downloading Artwork

When receiving a `show_artwork` command:

1. **Download the image** from the `art_url`
   - The URL is a direct link to the image file (PNG, JPEG, or GIF)
   - Example: `https://makapix.club/api/vault/{storage_key}.png`

2. **Cache the artwork** locally for offline use and faster access

3. **Insert it after the current index on the play queue**

4. **Swap-next to the new artwork**


### Recommended Caching Strategy

- Store downloaded artworks (posts) with their `storage_key` as the key, and a two-level hash-derived folder structure (hash the storage_key, derive the folder structure from the hash, the file name is the storage_key itself). This is to ensure that no single folder ever has too many files.
- Maintain a sizeable cache size (last 250 artworks)

---

## 6. Error Handling

### Provision Errors

| HTTP Status | Meaning | Action |
|-------------|---------|--------|
| 201 | Success | Store player_key, display code |
| 429 | Rate limited | Wait and retry |
| 500 | Server error | Retry with exponential backoff |

### MQTT Connection Errors

- **Connection refused:** Check player_key is valid, retry with backoff
- **Connection lost:** Reconnect automatically with exponential backoff
- **Subscribe failed:** Log error, retry subscription

### Command Errors

- **Invalid JSON:** Log and ignore the message
- **Unknown command_type:** Log and ignore (forward compatibility)
- **Download failed:** Log error, don't update display

---

## 7. State Diagram

```
┌─────────────────┐
│   POWER ON      │
└────────┬────────┘
         │
         ▼
┌─────────────────┐     No player_key stored
│ CHECK STORAGE   │─────────────────────────────┐
└────────┬────────┘                             │
         │ Has player_key                       │
         ▼                                      ▼
┌─────────────────┐                  ┌─────────────────┐
│ CONNECT MQTT    │                  │   PROVISION     │ <- DO PROVISION ONLY WHEN REQUESTED BY USER
└────────┬────────┘                  │   (POST /api/   │
         │                           │  player/provision)│
         │ Connected                 └────────┬────────┘
         ▼                                    │
┌─────────────────┐                           │
│ SUBSCRIBE TO    │                           │
│ COMMAND TOPIC   │◄──────────────────────────┘
└────────┬────────┘       Store player_key
         │
         ▼
┌─────────────────┐
│ REPORT STATUS   │
│ (online)        │
└────────┬────────┘
         │
         ▼
┌─────────────────┐     Command received
│ DISPLAY MODE    │◄────────────────────┐
│ (show artwork,  │                     │
│  wait for cmds) │─────────────────────┘
└────────┬────────┘     Execute command,
         │              report status
         │ Periodic
         ▼
┌─────────────────┐
│ REPORT STATUS   │
│ (every 30-60s)  │
└─────────────────┘
```

---

## 8. Example Implementation Pseudocode

```python
# Initialization
player_key = load_from_storage()

if player_key is None:
    # First boot - provision
    response = http_post("https://makapix.club/api/player/provision", {
        "device_model": "p3a",
        "firmware_version": FIRMWARE_VERSION
    })
    player_key = response["player_key"]
    registration_code = response["registration_code"]
    save_to_storage(player_key)
    
    # Display registration code
    display_registration_screen(registration_code, response["registration_code_expires_at"])

# Connect to MQTT
mqtt_client = MQTTClient(
    host="makapix.club",
    port=8883,
    username=player_key,
    password="",
    tls=True
)

# Set Last Will
mqtt_client.set_will(
    topic=f"makapix/player/{player_key}/status",
    payload=json.dumps({"player_key": player_key, "status": "offline"}),
    qos=1
)

mqtt_client.connect()

# Subscribe to commands
mqtt_client.subscribe(f"makapix/player/{player_key}/command", qos=1)

# Report online status
report_status("online")

# Main loop
while True:
    message = mqtt_client.receive(timeout=30)
    
    if message:
        command = json.loads(message.payload)
        handle_command(command)
    
    # Periodic status report
    if time_since_last_status() > 30:
        report_status("online")

def handle_command(command):
    if command["command_type"] == "swap_next":
        display_next_artwork()
    elif command["command_type"] == "swap_back":
        display_prev_artwork()
    elif command["command_type"] == "show_artwork":
        artwork = download_artwork(command["payload"]["art_url"])
        display_artwork(artwork)
        current_post_id = command["payload"]["post_id"]
    
    report_status("online")

def report_status(status):
    mqtt_client.publish(
        topic=f"makapix/player/{player_key}/status",
        payload=json.dumps({
            "player_key": player_key,
            "status": status,
            "current_post_id": current_post_id,
            "firmware_version": FIRMWARE_VERSION,
            "timestamp": datetime.utcnow().isoformat() + "Z"
        }),
        qos=1
    )
```

---

## 9. Testing

### Endpoints

- **API:** `https://makapix.club/api/`
- **MQTT Broker:** `makapix.club:8883` (TLS)

### Manual Testing Flow

1. Perform provision gesture (very long 10-second press on the screen), call provision endpoint, get registration code, display registration code on the player's screen
2. Log into Makapix Club website
3. Go to "My Players" page
4. Click "Register Player"
5. Enter the registration code and a name
6. Player can start receiving commands

---

## 10. Security Considerations

1. **Use TLS** for all MQTT connections (port 8883)
2. **Validate server certificates** using the CA certificate from `certs/player/makapix_ca.crt`
3. **Don't expose player_key** in logs or debug output visible to users
4. **Don't disable certificate verification** in production builds

---

## 11. Rate Limits

The server enforces rate limits on commands:
- **Per player:** 300 commands per minute
- **Per user (owner):** 1,000 commands per minute across all their players

These limits are enforced server-side; the player doesn't need to implement rate limiting.

---

## 12. Future Considerations

The following features are planned but not yet implemented:

- **Playlists:** Server-managed playlists of artworks
- **Certificate-based authentication:** TLS client certificates for stronger auth
- **OTA firmware updates:** Firmware update notifications via MQTT

The player firmware should be designed to easily accommodate these future features.

---

## Quick Reference

### API Endpoint
```
POST https://makapix.club/api/player/provision
```

### MQTT Connection
```
Host: makapix.club
Port: 8883 (TLS)
Username: {player_key}
Password: (empty)
CA Cert: certs/player/makapix_ca.crt
```

### MQTT Topics
```
Subscribe: makapix/player/{player_key}/command
Publish:   makapix/player/{player_key}/status
```

### Command Types
- `swap_next` - Next artwork in queue
- `swap_back` - Previous artwork in queue  
- `show_artwork` - Display specific artwork (includes `art_url`, `post_id`, `storage_key`)

### Status Fields
- `player_key` (required)
- `status`: "online" | "offline" (required)
- `current_post_id`: integer or null
- `firmware_version`: string
- `timestamp`: ISO 8601

