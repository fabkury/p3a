#ifndef MAKAPIX_CHANNEL_IMPL_H
#define MAKAPIX_CHANNEL_IMPL_H

#include "channel_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Makapix Channel - implements channel_interface
 * 
 * This channel connects to the Makapix Club server via MQTT to discover
 * and cache artworks. Artworks are stored locally in a vault with SHA256-
 * based naming for deduplication.
 * 
 * Key features:
 * - Paginated queries to remote server
 * - Local caching of artwork files and metadata
 * - Binary index file (index.bin) for fast loading
 * - Background refresh via MQTT
 * - Power-loss safe file operations
 */

/**
 * @brief Channel entry stored in index.bin (44 bytes, packed)
 */
typedef struct __attribute__((packed)) {
    uint8_t sha256[32];          // SHA256 hash of artwork file
    uint32_t created_at;         // Unix timestamp
    uint16_t flags;              // Filter flags (NSFW, format, etc.)
    uint8_t extension;           // File extension enum (0=webp, 1=gif, 2=png, 3=jpg)
    uint8_t reserved[5];         // Reserved for future use
} makapix_channel_entry_t;

_Static_assert(sizeof(makapix_channel_entry_t) == 44, "Channel entry must be 44 bytes");

/**
 * @brief Create a new Makapix channel
 * 
 * @param channel_id UUID of the channel (e.g., "abc123-def456-...")
 * @param name Display name for the channel
 * @param vault_path Base path for vault storage (e.g., "/sdcard/vault")
 * @param channels_path Base path for channel data (e.g., "/sdcard/channels")
 * @return Channel handle or NULL on failure
 */
channel_handle_t makapix_channel_create(const char *channel_id, 
                                         const char *name,
                                         const char *vault_path,
                                         const char *channels_path);

/**
 * @brief Get channel ID
 * 
 * @param channel Channel handle
 * @return Channel ID string or NULL
 */
const char *makapix_channel_get_id(channel_handle_t channel);

/**
 * @brief Check if a background refresh is in progress
 * 
 * @param channel Channel handle
 * @return true if refresh is ongoing
 */
bool makapix_channel_is_refreshing(channel_handle_t channel);

#ifdef __cplusplus
}
#endif

#endif // MAKAPIX_CHANNEL_IMPL_H

