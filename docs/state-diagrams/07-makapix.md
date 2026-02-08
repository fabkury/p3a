# Makapix State Machine

Manages the Makapix Cloud connection lifecycle: provisioning, MQTT connection, and registration validity. Defined in `components/makapix/makapix.h` as `makapix_state_t`.

## States

| State | Enum | Description |
|-------|------|-------------|
| Idle | `MAKAPIX_STATE_IDLE` | No player_key; waiting for user to initiate provisioning |
| Provisioning | `MAKAPIX_STATE_PROVISIONING` | HTTP provisioning request in progress |
| Show Code | `MAKAPIX_STATE_SHOW_CODE` | Displaying 6-character registration code on screen |
| Connecting | `MAKAPIX_STATE_CONNECTING` | MQTT client connecting to broker |
| Connected | `MAKAPIX_STATE_CONNECTED` | Normal operation; MQTT active |
| Disconnected | `MAKAPIX_STATE_DISCONNECTED` | MQTT lost; reconnection in progress |
| Registration Invalid | `MAKAPIX_STATE_REGISTRATION_INVALID` | Credentials rejected by server (3+ TLS auth failures) |

## Diagram

```mermaid
stateDiagram-v2
    [*] --> Idle : makapix_init()\n(no stored credentials)
    [*] --> Connecting : makapix_init()\n(credentials exist + WiFi)

    state "Provisioning Flow" as ProvFlow {
        Idle --> Provisioning : Long press gesture\nmakapix_start_provisioning()
        Provisioning --> ShowCode : HTTP success\n(player_key + code received)
        Provisioning --> Idle : HTTP failure\nor cancelled
        ShowCode --> Connecting : Credentials polled OK\n(certificates received)
        ShowCode --> Idle : Timeout (15 min)\nor cancelled
    }

    state "MQTT Lifecycle" as MQTTFlow {
        Connecting --> Connected : MQTT_EVENT_CONNECTED
        Connecting --> Disconnected : Connection failure

        Connected --> Disconnected : MQTT_EVENT_DISCONNECTED

        Disconnected --> Connecting : Reconnection attempt\n(WiFi available + auth OK)
        Disconnected --> RegistrationInvalid : 3+ TLS auth failures
    }

    RegistrationInvalid --> Idle : User re-provisions\nmakapix_start_provisioning()

    note right of ShowCode
        Polls /api/player/{key}/credentials
        every 3 seconds (up to 300 polls)
    end note

    note right of Connected
        Subscribes to command/response topics
        Starts periodic status publishing
    end note
```

## Provisioning Flow Detail

```mermaid
sequenceDiagram
    participant User
    participant p3a
    participant Makapix Cloud

    User->>p3a: Long press (10s)
    p3a->>p3a: Enter PROVISIONING state
    p3a->>Makapix Cloud: POST /api/player/provision
    Makapix Cloud-->>p3a: {player_key, code}
    p3a->>p3a: Enter SHOW_CODE state
    p3a->>p3a: Display code on screen

    loop Every 3 seconds (up to 15 min)
        p3a->>Makapix Cloud: GET /api/player/{key}/credentials
        alt Credentials ready
            Makapix Cloud-->>p3a: {cert, private_key, broker_url}
            p3a->>p3a: Save to NVS
            p3a->>p3a: Enter CONNECTING state
        else Not yet registered
            Makapix Cloud-->>p3a: 404 / pending
        end
    end

    p3a->>Makapix Cloud: MQTT Connect (TLS mutual auth)
    Makapix Cloud-->>p3a: CONNACK
    p3a->>p3a: Enter CONNECTED state
```

## TLS Authentication Failure Detection

The Makapix component tracks consecutive TLS handshake failures:

| Failure Count | Behavior |
|--------------|----------|
| 1-2 | Normal reconnection retry |
| 3+ | Transition to `REGISTRATION_INVALID`; stop reconnecting |

This detects cases where the device's certificate has been revoked or the registration is no longer valid on the server side.

Recovery requires the user to re-provision the device (long press gesture).

## Reconnection Behavior

When in `DISCONNECTED` state:
1. A background reconnection task periodically attempts to reconnect
2. Reconnection requires: WiFi connected + credentials available + auth failures < 3
3. On success: transitions to `CONNECTING` → `CONNECTED`
4. On TLS failure: increments auth failure counter

## Integration with p3a Core

- `makapix_start_provisioning()` triggers `p3a_state_enter_provisioning()`
- MQTT connected/disconnected events update `p3a_connectivity_level_t`
- Provisioning cancel returns to `P3A_STATE_ANIMATION_PLAYBACK`

## Source Files

- `components/makapix/makapix.h` - State enum and public API
- `components/makapix/makapix.c` - Main state logic and provisioning
- `components/makapix/makapix_mqtt.c` - MQTT client management
