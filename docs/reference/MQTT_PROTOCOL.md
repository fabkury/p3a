# Makapix Club MQTT Protocol Documentation

**Version:** 1.0  
**Last Updated:** December 2025  
**Target Audience:** Client developers (ESP32-P4, microcontrollers, web, and mobile applications)

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Connection Methods](#connection-methods)
4. [Authentication](#authentication)
5. [Topic Hierarchy](#topic-hierarchy)
6. [Protocol Specifications](#protocol-specifications)
   - [Player-to-Server Requests](#player-to-server-requests)
   - [Server-to-Player Commands](#server-to-player-commands)
   - [Status Updates](#status-updates)
   - [Notifications](#notifications)
7. [REST API Integration](#rest-api-integration)
8. [Security](#security)
9. [Rate Limiting](#rate-limiting)
10. [Error Handling](#error-handling)
11. [Best Practices](#best-practices)
12. [Examples](#examples)

---

## Overview

Makapix Club uses MQTT as the primary protocol for real-time communication between the server and connected clients (physical players, web clients, and mobile apps). The protocol supports:

- **Bidirectional Communication**: Players can query content and submit interactions; the server can send commands and notifications
- **Multiple Client Types**: Physical players (ESP32-P4, microcontrollers), web browsers, mobile apps
- **Secure Authentication**: mTLS for players, password authentication for web/internal services
- **Real-time Notifications**: New post alerts, category promotions, follower updates
- **Request/Response Pattern**: Asynchronous request-response with correlation IDs
- **Status Tracking**: Online/offline status, current artwork display, heartbeats

### Key Features

- **QoS Levels**: Supports QoS 0 (at-most-once) and QoS 1 (at-least-once) delivery
- **Three Connection Methods**: mTLS (8883), password auth (1883), WebSocket (9001)
- **Rich Query API**: Filter posts by channel, sort by various criteria, paginate results
- **Interaction Tracking**: View events, reactions, comments
- **Remote Control**: Server can send commands to players (show artwork, navigate)
- **Permission Inheritance**: Players inherit access rights from their owner's account

---

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                   Client Devices                             │
│  - Physical Players (ESP32-P4, etc.) - mTLS                 │
│  - Web Browsers - WebSocket                                  │
│  - Mobile Apps - mTLS or WebSocket                          │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              Mosquitto MQTT Broker                           │
│  - Port 1883: Internal (password auth)                      │
│  - Port 8883: mTLS (physical players)                       │
│  - Port 9001: WebSocket (web clients)                       │
└───────────────┬──────────────────────┬──────────────────────┘
                │                      │
    ┌───────────▼──────────┐   ┌──────▼─────────────┐
    │ Request Subscriber   │   │ Status Subscriber   │
    │ (player_requests.py) │   │ (player_status.py)  │
    └───────────┬──────────┘   └──────┬─────────────┘
                │                     │
                └──────────┬──────────┘
                           ▼
                ┌────────────────────┐
                │  FastAPI Server    │
                │  - Authentication  │
                │  - Business Logic  │
                │  - Database Access │
                └──────────┬─────────┘
                           ▼
                ┌────────────────────┐
                │  PostgreSQL DB     │
                │  - Users, Players  │
                │  - Posts, Comments │
                │  - Reactions, Views│
                └────────────────────┘
```

### Message Flow Patterns

#### Request-Response Pattern (Player-to-Server)
```
Player                          MQTT Broker               Server
  |                                  |                      |
  |-- Publish Request -------------->|                      |
  |   (makapix/player/{key}/request/{id})                  |
  |                                  |-- Forward ---------> |
  |                                  |                      |
  |                                  |            Process & Query DB
  |                                  |                      |
  |                                  |<-- Publish Response -|
  |                                  |   (makapix/player/{key}/response/{id})
  |<-- Receive Response -------------|                      |
```

#### Command Pattern (Server-to-Player)
```
Server                         MQTT Broker                Player
  |                                  |                      |
  |-- Publish Command -------------->|                      |
  |   (makapix/player/{key}/command) |                      |
  |                                  |-- Forward ---------> |
  |                                  |                      |
  |                                  |            Execute Command
```

#### Notification Pattern (Server Broadcast)
```
Server                         MQTT Broker           Subscribers
  |                                  |                      |
  |-- Publish Notification --------->|                      |
  |   (makapix/post/new/user/{uid})  |                      |
  |                                  |-- Broadcast -------> |
  |                                  |                   (All followers)
```

---

## Connection Methods

Makapix Club MQTT broker supports three connection methods:

### 1. mTLS (Port 8883) - **Recommended for Physical Players**

**Use Case**: Secure connections for physical players (ESP32-P4, microcontrollers)

**Features**:
- Mutual TLS authentication using client certificates
- Certificate-based identity (CN = player_key UUID)
- Strong cryptographic security
- No password required (certificate is the credential)

**Configuration**:
```
Host: dev.makapix.club (or production host)
Port: 8883
Protocol: MQTT v5
TLS: Required
Client Certificate: Required (issued by Makapix CA)
TLS Version: TLS 1.2 or higher
```

**Requirements**:
- CA certificate (ca.crt)
- Client certificate (client.crt) with CN=player_key
- Client private key (client.key)

### 2. Password Authentication (Port 1883) - **Internal Use Only**

**Use Case**: Internal API server communication within Docker network

**Features**:
- Username/password authentication
- No encryption (relies on network isolation)
- Used by API server for publishing

**Configuration**:
```
Host: mqtt (internal Docker hostname)
Port: 1883
Protocol: MQTT v5
TLS: None
Username: api-server (or player_key for players)
Password: Configured in password file
```

**Note**: This port is not exposed publicly and should only be used within secure internal networks.

### 3. WebSocket (Port 9001) - **Web Browsers and Mobile Apps**

**Use Case**: Browser-based clients, JavaScript applications

**Features**:
- WebSocket transport over MQTT
- Password authentication
- Compatible with web browsers
- CORS-friendly

**Configuration**:
```
URL: ws://dev.makapix.club:9001 (or wss:// for TLS)
Protocol: MQTT over WebSocket
Username: user_id
Password: Authentication token (from REST API)
```

**Usage**:
```javascript
import mqtt from 'mqtt';

const client = mqtt.connect('ws://dev.makapix.club:9001', {
  clientId: `web-client-${userId}`,
  username: userId,
  password: authToken,
  protocol: 'mqttv5',
});
```

---

## Authentication

### Player Authentication Flow

Physical players use a two-step provisioning and registration process:

#### Step 1: Provision (Device → REST API)

Device calls REST endpoint to obtain player_key and registration code:

```http
POST /player/provision
Content-Type: application/json

{
  "device_model": "ESP32-P4-Function-EV-Board",
  "firmware_version": "1.0.0"
}
```

**Response**:
```json
{
  "player_key": "550e8400-e29b-41d4-a716-446655440000",
  "registration_code": "ABC123",
  "registration_code_expires_at": "2025-12-09T02:00:00Z",
  "mqtt_broker": {
    "host": "dev.makapix.club",
    "port": 8883
  }
}
```

The `registration_code` is a 6-character alphanumeric code that expires in 15 minutes.

#### Step 2: Register (Owner → REST API)

User registers the device to their account using the registration code:

```http
POST /player/register
Authorization: Bearer <user_token>
Content-Type: application/json

{
  "registration_code": "ABC123",
  "name": "Living Room Player"
}
```

**Response**:
```json
{
  "id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "player_key": "550e8400-e29b-41d4-a716-446655440000",
  "owner_id": "3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "name": "Living Room Player",
  "device_model": "ESP32-P4-Function-EV-Board",
  "firmware_version": "1.0.0",
  "registration_status": "registered",
  "registered_at": "2025-12-09T01:30:00Z",
  "cert_expires_at": "2026-12-09T01:30:00Z"
}
```

#### Step 3: Download Certificates (Device → REST API)

Device downloads TLS certificates using player_key:

```http
GET /player/{player_key}/credentials
```

**Response**:
```json
{
  "ca_pem": "-----BEGIN CERTIFICATE-----\n...",
  "cert_pem": "-----BEGIN CERTIFICATE-----\n...",
  "key_pem": "-----BEGIN PRIVATE KEY-----\n...",
  "broker": {
    "host": "dev.makapix.club",
    "port": 8883
  }
}
```

#### Step 4: Connect via MQTT

Device connects using mTLS with downloaded certificates:

```python
import paho.mqtt.client as mqtt

client = mqtt.Client(
    client_id=f"player-{player_key}",
    protocol=mqtt.MQTTv5
)

client.tls_set(
    ca_certs="ca.crt",
    certfile="client.crt",
    keyfile="client.key"
)

client.connect("dev.makapix.club", 8883, keepalive=60)
```

### Web Client Authentication

Web clients authenticate using WebSocket with user credentials:

1. User logs in via REST API and obtains auth token
2. Web client connects to WebSocket port 9001 with user_id and token
3. Server validates token and establishes session

---

## Topic Hierarchy

### Topic Naming Convention

All Makapix MQTT topics follow this pattern:

```
makapix/{category}/{subcategory}/{identifiers}
```

### Topic Structure Overview

```
makapix/
├── player/
│   └── {player_key}/
│       ├── request/{request_id}      # Player → Server requests
│       ├── response/{request_id}     # Server → Player responses
│       ├── command                    # Server → Player commands
│       └── status                     # Player → Server status updates
│
└── post/
    └── new/
        ├── {post_id}                  # Generic new post notification
        ├── user/{user_id}/{post_id}   # New post from followed user
        └── category/{category}/{post_id}  # New promoted post in category
```

### Topic Details

#### Player Request Topics

**Pattern**: `makapix/player/{player_key}/request/{request_id}`

- **Direction**: Player → Server
- **QoS**: 1 (at-least-once)
- **Retained**: No
- **Payload**: JSON request object
- **Wildcards**: Server subscribes to `makapix/player/+/request/+`

**Example**: `makapix/player/550e8400-e29b-41d4-a716-446655440000/request/req-001`

#### Player Response Topics

**Pattern**: `makapix/player/{player_key}/response/{request_id}`

- **Direction**: Server → Player
- **QoS**: 1 (at-least-once)
- **Retained**: No
- **Payload**: JSON response object
- **Wildcards**: Player subscribes to `makapix/player/{player_key}/response/#`

**Example**: `makapix/player/550e8400-e29b-41d4-a716-446655440000/response/req-001`

#### Player Command Topic

**Pattern**: `makapix/player/{player_key}/command`

- **Direction**: Server → Player
- **QoS**: 1 (at-least-once)
- **Retained**: No
- **Payload**: JSON command object
- **Wildcards**: Player subscribes to `makapix/player/{player_key}/command`

**Example**: `makapix/player/550e8400-e29b-41d4-a716-446655440000/command`

#### Player Status Topic

**Pattern**: `makapix/player/{player_key}/status`

- **Direction**: Player → Server
- **QoS**: 1 (at-least-once)
- **Retained**: No (last status kept in database, not MQTT)
- **Payload**: JSON status object
- **Wildcards**: Server subscribes to `makapix/player/+/status`

**Example**: `makapix/player/550e8400-e29b-41d4-a716-446655440000/status`

#### Post Notification Topics

**Pattern**: Various (see below)

1. **Generic new post**: `makapix/post/new/{post_id}`
   - Broadcast notification for monitoring/debugging

2. **User-specific new post**: `makapix/post/new/user/{follower_id}/{post_id}`
   - Sent to each follower when an artist posts new content
   - QoS: 1, Retained: No

3. **Category promotion**: `makapix/post/new/category/{category}/{post_id}`
   - Sent when post is promoted to a category (e.g., "daily's-best")
   - QoS: 1, Retained: No

---

## Protocol Specifications

### Player-to-Server Requests

For complete details on player-to-server request/response protocol, please refer to [MQTT_PLAYER_API.md](./MQTT_PLAYER_API.md), which provides:

- Detailed request/response schemas for all operations
- Field descriptions and constraints
- Example requests and responses
- Error handling specifics
- Python client implementation examples

**Supported Operations Summary**:

1. **Query Posts** (`query_posts`): Query posts from various channels with filtering, sorting, and pagination
2. **Submit View** (`submit_view`): Record view events with intent classification
3. **Submit Reaction** (`submit_reaction`): Add emoji reactions to posts
4. **Revoke Reaction** (`revoke_reaction`): Remove emoji reactions
5. **Get Comments** (`get_comments`): Retrieve comments with pagination

All operations require:
- `request_id`: Unique identifier for correlation
- `request_type`: Operation name
- `player_key`: UUID for authentication

All responses include:
- `request_id`: Correlated with request
- `success`: Boolean indicating success/failure
- `error`: Error message if `success` is false

---

### Server-to-Player Commands

The server can send commands to players for remote control operations.

#### Command Message Format

**Topic**: `makapix/player/{player_key}/command`

**Message Structure**:
```json
{
  "command_id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "command_type": "show_artwork",
  "payload": {
    // command-specific data
  },
  "timestamp": "2025-12-09T01:30:00Z"
}
```

**Common Fields**:
- `command_id`: UUID, unique command identifier
- `command_type`: String, command type
- `payload`: Object, command-specific data
- `timestamp`: ISO 8601 timestamp

#### Supported Commands

##### 1. Show Artwork

Display a specific artwork on the player.

**Command Type**: `show_artwork`

**Payload**:
```json
{
  "post_id": 123,
  "storage_key": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "art_url": "https://cdn.makapix.club/art/...",
  "canvas": "64x64"
}
```

**Fields**:
- `post_id`: Integer, post ID
- `storage_key`: UUID, storage identifier
- `art_url`: String, URL to artwork file
- `canvas`: String, canvas size

**Expected Player Behavior**:
1. Download artwork from `art_url`
2. Display artwork on screen
3. Update current_post_id in status updates

##### 2. Swap Next

Move to next artwork in current playlist/queue.

**Command Type**: `swap_next`

**Payload**: `{}` (empty)

**Expected Player Behavior**:
1. Move to next post in local queue
2. Display next artwork
3. Update status

##### 3. Swap Back

Move to previous artwork in playlist/queue.

**Command Type**: `swap_back`

**Payload**: `{}` (empty)

**Expected Player Behavior**:
1. Move to previous post in local queue
2. Display previous artwork
3. Update status

---

### Status Updates

Players send periodic status updates to inform the server of their current state.

#### Status Message Format

**Topic**: `makapix/player/{player_key}/status`

**Message Structure**:
```json
{
  "player_key": "550e8400-e29b-41d4-a716-446655440000",
  "status": "online",
  "current_post_id": 123,
  "firmware_version": "1.0.0"
}
```

**Fields**:
- `player_key`: UUID, player's key
- `status`: String, connection status
  - `"online"`: Player is connected and active
  - `"offline"`: Player is disconnecting gracefully
- `current_post_id`: Integer or null, currently displayed post ID
- `firmware_version`: String (optional), firmware version

#### Status Update Behavior

**Frequency**: 
- Send status update every 60 seconds (heartbeat)
- Send immediately on status change (e.g., artwork change)

**Server Updates**:
- Updates `Player.connection_status` in database
- Updates `Player.last_seen_at` timestamp
- Updates `Player.current_post_id` if provided
- Updates `Player.firmware_version` if provided

**Example Status Update Flow**:
```python
import time
import json

while connected:
    status = {
        "player_key": str(player_key),
        "status": "online",
        "current_post_id": current_post,
        "firmware_version": "1.0.0"
    }
    client.publish(
        f"makapix/player/{player_key}/status",
        json.dumps(status),
        qos=1
    )
    time.sleep(60)  # Wait 60 seconds before next heartbeat
```

---

### Notifications

The server publishes notifications for real-time events that clients may subscribe to.

#### Post Notification Payload

**Common Structure**:
```json
{
  "post_id": 123,
  "owner_id": "3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "owner_handle": "artist123",
  "title": "Cool Pixel Art",
  "art_url": "https://cdn.makapix.club/art/...",
  "canvas": "64x64",
  "promoted_category": null,
  "created_at": "2025-12-09T01:30:00Z"
}
```

**Fields**:
- `post_id`: Integer, post ID
- `owner_id`: UUID, creator's user ID
- `owner_handle`: String, creator's handle
- `title`: String, post title
- `art_url`: String, URL to artwork
- `canvas`: String, canvas size
- `promoted_category`: String or null, promotion category if any
- `created_at`: ISO 8601 timestamp

#### Notification Types

##### 1. New Post from Followed User

**Topic**: `makapix/post/new/user/{follower_id}/{post_id}`

Published when an artist posts new content, sent to each follower.

**Subscription**:
```python
# Subscribe to all new posts from artists you follow
client.subscribe(f"makapix/post/new/user/{user_id}/#", qos=1)
```

**Use Case**: Web/mobile clients can display real-time notifications when followed artists post new content.

##### 2. Category Promotion

**Topic**: `makapix/post/new/category/{category}/{post_id}`

Published when a post is promoted to a category (e.g., "daily's-best").

**Subscription**:
```python
# Subscribe to all "daily's-best" promotions
client.subscribe("makapix/post/new/category/daily's-best/#", qos=1)
```

**Use Case**: Clients can notify users when their posts are featured or when new featured content is available.

##### 3. Generic New Post

**Topic**: `makapix/post/new/{post_id}`

Broadcast notification for all new posts (monitoring/debugging).

**Note**: This is primarily for system monitoring and not intended for end-user clients.

---

## REST API Integration

While MQTT handles real-time communication, the REST API is used for device provisioning, registration, and management.

### Player Lifecycle

```
┌─────────────┐
│  Provision  │  Device → POST /player/provision
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  Register   │  Owner → POST /player/register
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ Get Certs   │  Device → GET /player/{key}/credentials
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ MQTT Connect│  Device uses mTLS
└──────┬──────┘
       │
       ▼
┌─────────────┐
│   Active    │  Request/Response, Commands, Status
└─────────────┘
```

### Key REST Endpoints

#### Player Management

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/player/provision` | Device provisions new player |
| POST | `/player/register` | Owner registers player to account |
| GET | `/player/{player_key}/credentials` | Device downloads TLS certificates |
| GET | `/u/{sqid}/player` | List user's players |
| GET | `/u/{sqid}/player/{player_id}` | Get player details |
| PATCH | `/u/{sqid}/player/{player_id}` | Update player (name) |
| DELETE | `/u/{sqid}/player/{player_id}` | Remove player |
| POST | `/u/{sqid}/player/{player_id}/command` | Send command to player |
| POST | `/u/{sqid}/player/command/all` | Send command to all user's players |
| POST | `/u/{sqid}/player/{player_id}/renew-cert` | Renew TLS certificate |

#### MQTT Configuration

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/mqtt/bootstrap` | Get MQTT broker connection info |
| POST | `/mqtt/demo` | Publish demo message (dev only) |

### Authentication

REST endpoints require Bearer token authentication (except provision and credentials endpoints which use player_key).

```http
Authorization: Bearer <user_jwt_token>
```

---

## Security

### Transport Security

#### mTLS (Port 8883)

- **Certificate Authority**: Makapix Club CA
- **Client Authentication**: Required (CN = player_key)
- **TLS Version**: TLS 1.2 or higher
- **Certificate Validity**: 365 days
- **Renewal**: Available within 30 days of expiry

**Certificate Chain**:
```
Makapix Club Root CA
  └── Server Certificate (broker)
  └── Client Certificate (player)
```

#### WebSocket (Port 9001)

- **Authentication**: Password-based (username/password)
- **TLS**: Optional (use wss:// for encrypted WebSocket)
- **CORS**: Configured for web browser compatibility

### Authorization

Players inherit permissions from their owner's user account:

- **View Access**: Respects post visibility settings
- **Reaction Limits**: Max 5 reactions per user per post
- **Rate Limits**: Player-specific and user-level limits
- **Content Filtering**: Hidden/moderated content filtered

### Data Privacy

- **View Tracking**: Uses hashed identifiers
- **IP Addresses**: Not exposed to clients
- **Owner Views**: Excluded from tracking (no self-views)
- **Anonymous Comments**: Author handle can be null

### Input Validation

- **Parameterized Queries**: All database queries use bound parameters
- **Schema Validation**: Pydantic schemas validate all requests
- **Emoji Validation**: Length and format checks
- **Cursor Validation**: Pagination cursors validated

---

## Rate Limiting

### Player-Level Limits

**Commands**: 300 per minute per player

**Applies to**:
- All player-to-server requests
- Server-to-player commands (when initiated by owner)

### User-Level Limits

**Commands**: 1000 per minute per user (across all players)

**Applies to**:
- All operations from user's players combined
- Direct user actions via REST API

### Handling Rate Limits

**Response**: 429 Too Many Requests (REST) or error response (MQTT)

**Retry Strategy**:
1. Implement exponential backoff
2. Start with 1 second delay
3. Double delay on each retry (max 60 seconds)
4. Include jitter to avoid thundering herd

**Example**:
```python
import time
import random

def retry_with_backoff(func, max_retries=5):
    delay = 1
    for attempt in range(max_retries):
        try:
            return func()
        except RateLimitException:
            if attempt == max_retries - 1:
                raise
            jitter = random.uniform(0, 0.1 * delay)
            time.sleep(delay + jitter)
            delay = min(delay * 2, 60)
```

---

## Error Handling

### Error Response Format

```json
{
  "request_id": "req-001",
  "success": false,
  "error": "Human-readable error message",
  "error_code": "error_code_constant"
}
```

### Common Error Codes

| Error Code | Description | Cause |
|------------|-------------|-------|
| `authentication_failed` | Player not registered or invalid player_key | Invalid or expired player_key |
| `not_found` | Resource not found | Invalid post ID or comment ID |
| `invalid_request` | Malformed request | Missing required fields or invalid format |
| `invalid_emoji` | Emoji validation failed | Emoji too long or invalid format |
| `reaction_limit_exceeded` | Max reactions exceeded | User already has 5 reactions on post |
| `internal_error` | Server error | Database or processing error |
| `unknown_request_type` | Unsupported operation | Invalid request_type value |

### Error Handling Best Practices

1. **Always Check `success` Field**: Don't assume success
2. **Log Error Details**: Log both `error` and `error_code`
3. **Implement Retry Logic**: For transient errors (e.g., `internal_error`)
4. **Validate Before Sending**: Client-side validation reduces errors
5. **Handle Timeouts**: Implement 30-second timeout for responses

**Example**:
```python
import json
import logging

logger = logging.getLogger(__name__)

def handle_response(response_payload):
    response = json.loads(response_payload)
    
    if not response.get("success", False):
        error = response.get("error", "Unknown error")
        error_code = response.get("error_code", "unknown")
        
        if error_code == "authentication_failed":
            # Re-provision and register
            reprovision_player()
        elif error_code == "rate_limit_exceeded":
            # Implement backoff
            time.sleep(60)
        else:
            # Log and report
            logger.error(f"Request failed: {error} ({error_code})")
        
        return None
    
    return response
```

---

## Best Practices

### For Player Developers

#### Connection Management

1. **Generate Unique Client IDs**: Use player_key in client ID
   ```python
   client_id = f"player-{player_key}"
   ```

2. **Implement Reconnection Logic**: Handle disconnections gracefully
   ```python
   def on_disconnect(client, userdata, rc):
       if rc != 0:
           logger.warning(f"Unexpected disconnect (rc={rc}), reconnecting...")
           reconnect_with_backoff()
   ```

3. **Use QoS 1 for Important Messages**: Ensure delivery
   ```python
   client.publish(topic, payload, qos=1)
   ```

4. **Set Keep-Alive**: Use 60-second keep-alive interval
   ```python
   client.connect(host, port, keepalive=60)
   ```

#### Request/Response Pattern

1. **Subscribe Before Publishing**: Subscribe to response topic first
   ```python
   client.subscribe(f"makapix/player/{player_key}/response/#", qos=1)
   time.sleep(1)  # Wait for subscription confirmation
   # Now safe to send requests
   ```

2. **Use Unique Request IDs**: UUIDs recommended
   ```python
   import uuid
   request_id = str(uuid.uuid4())
   ```

3. **Implement Timeouts**: 30-second timeout for responses
   ```python
   response = wait_for_response(request_id, timeout=30)
   if response is None:
       logger.error("Request timeout")
   ```

4. **Correlate Responses**: Match response request_id with sent request_id

#### Status Updates

1. **Send Periodic Heartbeats**: Every 60 seconds
2. **Update on State Changes**: Send status when artwork changes
3. **Include Firmware Version**: Helps server track devices

#### Optimization

1. **Cache Post Metadata Locally**: Reduce query frequency
2. **Batch Operations**: Queue multiple views/reactions
3. **Lazy Load Images**: Download artwork only when displaying
4. **Implement Local Queue**: Handle offline scenarios

### For Web/Mobile Developers

1. **Use WebSocket Connection**: Port 9001 for browsers
2. **Handle Notifications**: Subscribe to relevant topics
3. **Implement UI Updates**: Real-time notification display
4. **Graceful Degradation**: Fall back to REST API if MQTT unavailable

### For Server-Side Developers

1. **Validate All Inputs**: Use Pydantic schemas
2. **Check Permissions**: Enforce owner-based access control
3. **Log Commands**: Audit trail for debugging
4. **Rate Limit**: Protect against abuse

---

## Examples

### Complete ESP32-P4 MicroPython Example

See [MQTT_PLAYER_API.md](./MQTT_PLAYER_API.md) for detailed Python implementation examples with paho-mqtt library, including:
- Complete client setup with mTLS
- Request/response correlation
- All MQTT operations (query, view, reaction, comments)
- Status update loop
- Error handling

### JavaScript/TypeScript Web Client Example

```typescript
import mqtt from 'mqtt';

interface PostNotification {
  post_id: number;
  owner_id: string;
  owner_handle: string;
  title: string;
  art_url: string;
  canvas: string;
  promoted_category: string | null;
  created_at: string;
}

class MakapixWebClient {
  private client: mqtt.MqttClient;
  private userId: string;
  private callbacks: Map<string, (notification: PostNotification) => void>;

  constructor(userId: string, authToken: string, brokerUrl: string = 'ws://dev.makapix.club:9001') {
    this.userId = userId;
    this.callbacks = new Map();

    this.client = mqtt.connect(brokerUrl, {
      clientId: `web-client-${userId}`,
      username: userId,
      password: authToken,
      protocol: 'mqttv5',
    });

    this.client.on('connect', () => {
      console.log('Connected to MQTT broker');
      this.subscribeToNotifications();
    });

    this.client.on('message', (topic, message) => {
      this.handleMessage(topic, message);
    });

    this.client.on('error', (error) => {
      console.error('MQTT error:', error);
    });
  }

  private subscribeToNotifications(): void {
    // Subscribe to new posts from followed users
    const userTopic = `makapix/post/new/user/${this.userId}/#`;
    this.client.subscribe(userTopic, { qos: 1 }, (err) => {
      if (err) {
        console.error('Subscription error:', err);
      } else {
        console.log(`Subscribed to ${userTopic}`);
      }
    });

    // Subscribe to category promotions
    const categoryTopic = `makapix/post/new/category/daily's-best/#`;
    this.client.subscribe(categoryTopic, { qos: 1 }, (err) => {
      if (err) {
        console.error('Subscription error:', err);
      } else {
        console.log(`Subscribed to ${categoryTopic}`);
      }
    });
  }

  private handleMessage(topic: string, message: Buffer): void {
    try {
      const notification: PostNotification = JSON.parse(message.toString());

      // Determine notification type from topic
      if (topic.includes('/user/')) {
        this.triggerCallback('new_post_from_followed', notification);
      } else if (topic.includes('/category/')) {
        this.triggerCallback('category_promotion', notification);
      }
    } catch (error) {
      console.error('Error parsing notification:', error);
    }
  }

  private triggerCallback(type: string, notification: PostNotification): void {
    const callback = this.callbacks.get(type);
    if (callback) {
      callback(notification);
    }
  }

  public onNewPostFromFollowed(callback: (notification: PostNotification) => void): void {
    this.callbacks.set('new_post_from_followed', callback);
  }

  public onCategoryPromotion(callback: (notification: PostNotification) => void): void {
    this.callbacks.set('category_promotion', callback);
  }

  public disconnect(): void {
    this.client.end();
  }
}

// Usage
const client = new MakapixWebClient(userId, authToken);

client.onNewPostFromFollowed((notification) => {
  console.log(`New post from ${notification.owner_handle}: ${notification.title}`);
  // Show notification in UI
});

client.onCategoryPromotion((notification) => {
  console.log(`Featured: ${notification.title} by ${notification.owner_handle}`);
  // Show featured notification
});
```

---

## Appendix

### MQTT Version

Makapix Club uses **MQTT v5** (also compatible with MQTT v3.1.1).

### QoS Guidelines

- **QoS 0** (at-most-once): Not currently used
- **QoS 1** (at-least-once): Used for all player requests, responses, commands, and notifications
- **QoS 2** (exactly-once): Not currently used

### Message Retention

- **Responses**: Not retained (transient, correlated with requests)
- **Commands**: Not retained (one-time instructions)
- **Status**: Not retained (database is source of truth)
- **Notifications**: Not retained (real-time events only)

### Testing

For testing and validation, use the provided script:

```bash
python3 scripts/validate_mqtt_player_api.py \
    --player-key "your-player-uuid" \
    --host "localhost" \
    --port 1883 \
    --post-id 123
```

### Support

For questions or issues with the MQTT protocol:

1. Review this documentation
2. Check [MQTT_PLAYER_API.md](./MQTT_PLAYER_API.md) for detailed operation specs
3. Review existing tests in `api/tests/test_mqtt_player_requests.py`
4. Review source code in `api/app/mqtt/`
5. Contact the Makapix Club development team

### Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | December 2025 | Initial comprehensive documentation |

---

**End of Documentation**
