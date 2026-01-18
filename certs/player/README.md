# Player Certificates

This folder contains certificates that **physical players** need to connect to Makapix Club services.

## Files

| File | Format | Description |
|------|--------|-------------|
| `makapix_ca.crt` | PEM | CA certificate for verifying the MQTT broker's identity |
| `makapix_ca.inc` | C include | Same certificate in C string format for embedding in firmware |

## Usage

### For TLS Connection to MQTT Broker

Players use the CA certificate to verify the MQTT broker's server certificate during TLS handshake.

**Connection details:**
- **Host:** `makapix.club`
- **Port:** `8883` (TLS)
- **CA Certificate:** Use `makapix_ca.crt` or `makapix_ca.inc`

### Embedding in Firmware (C/C++)

Include the `.inc` file in your source code:

```c
const char* MAKAPIX_CA_CERT = 
#include "makapix_ca.inc"
;
```

### For HTTPS API Calls

The same CA certificate can be used to verify HTTPS connections to the API:
- `https://makapix.club/api/`

## Regenerating Certificates

If you need to regenerate these files (e.g., after CA renewal):

```bash
# Copy CA cert from server
cp ../mqtt/certs/ca.crt makapix_ca.crt

# Convert to .inc format
python3 -c "
with open('makapix_ca.crt', 'r') as f:
    lines = f.read().strip().split('\n')
with open('makapix_ca.inc', 'w') as f:
    for line in lines:
        f.write(f'\"{line}\\\\n\"\n')
"
```

## Security Notes

- The CA certificate is **public** - it's safe to embed in firmware
- Never embed private keys in firmware source code
- These certificates verify the server's identity, not the player's identity

