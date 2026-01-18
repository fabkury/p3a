# Makapix Club Certificates

This directory contains TLS certificates organized by which entity uses them.

## Directory Structure

```
certs/
├── README.md           # This file
├── player/             # Certificates for physical player devices
│   ├── README.md       # Player-specific documentation
│   ├── makapix_ca.crt  # CA certificate (PEM format)
│   └── makapix_ca.inc  # CA certificate (C include format)
└── server/             # Server certificate documentation
    └── README.md       # Server-specific documentation
```

## Quick Reference

### For Physical Players (p3a, etc.)

Players need the **CA certificate** to verify the MQTT broker:

- **PEM format:** `player/makapix_ca.crt`
- **C include:** `player/makapix_ca.inc`

See `player/README.md` for embedding instructions.

### For Server/MQTT Broker

Server certificates are in `mqtt/certs/` (not this folder).

See `server/README.md` for regeneration instructions.

## TLS Connection Flow

```
Player                           MQTT Broker
  |                                   |
  |------ TLS ClientHello ----------->|
  |                                   |
  |<----- TLS ServerHello ------------|
  |<----- Server Certificate ---------|  (mqtt/certs/server.crt)
  |                                   |
  | [Verify server cert using         |
  |  player/makapix_ca.crt]           |
  |                                   |
  |------ TLS Finished -------------->|
  |<----- TLS Finished ---------------|
  |                                   |
  | [TLS connection established]      |
  |                                   |
  |------ MQTT CONNECT --------------->|  (username = player_key)
  |<----- MQTT CONNACK ---------------|
```

## Connection Details

| API Host | MQTT Host | Port |
|----------|-----------|------|
| makapix.club | makapix.club | 8883 |
