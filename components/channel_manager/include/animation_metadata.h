#ifndef ANIMATION_METADATA_H
#define ANIMATION_METADATA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Metadata for a playing animation
 * 
 * When animation_player is playing an animation:
 * - filepath is always valid (non-NULL) because there must be an animation file
 * - If JSON sidecar exists and was parsed successfully, has_metadata = true
 * - If no sidecar or parse failed, has_metadata = false and placeholder fields are NULL/0/false
 * 
 * The JSON sidecar file has the same stem as the animation file but with _meta.json suffix:
 * e.g. animation_2025_12_05.webp -> animation_2025_12_05_meta.json
 */
typedef struct {
    // Always present when playing an animation
    char *filepath;           // Full path to animation file (always non-NULL when playing)
    
    // Metadata state
    bool has_metadata;        // True if JSON sidecar was successfully loaded
    
    // Placeholder metadata fields
    // When has_metadata == false, these are NULL/0/false
    // When has_metadata == true, these may be populated from JSON (or defaults if field missing)
    char *field1;             // String field (placeholder - future: title, artist, etc.)
    int32_t field2;           // Integer field (placeholder - future: likes, year, etc.)
    bool field3;              // Boolean field (placeholder - future: nsfw, featured, etc.)
} animation_metadata_t;

/**
 * @brief Initialize metadata structure
 * 
 * Sets all fields to NULL/0/false.
 * 
 * @param meta Metadata structure to initialize
 */
void animation_metadata_init(animation_metadata_t *meta);

/**
 * @brief Free metadata resources
 * 
 * Frees allocated strings (filepath, field1) and resets structure.
 * 
 * @param meta Metadata structure to free
 */
void animation_metadata_free(animation_metadata_t *meta);

/**
 * @brief Set the filepath for an animation
 * 
 * This should always be called when loading an animation.
 * The filepath is duplicated internally.
 * 
 * @param meta Metadata structure
 * @param filepath Path to animation file
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t animation_metadata_set_filepath(animation_metadata_t *meta, const char *filepath);

/**
 * @brief Load metadata from JSON sidecar file
 * 
 * Looks for a JSON sidecar file with the same stem as the animation file.
 * For example: <animations_dir>/art.webp -> <animations_dir>/art_meta.json
 * 
 * If the sidecar exists and is valid JSON, populates the metadata fields
 * and sets has_metadata = true. If no sidecar exists or parsing fails,
 * has_metadata remains false and placeholder fields remain NULL/0/false.
 * 
 * @param meta Metadata structure (filepath must already be set)
 * @return ESP_OK if sidecar found and parsed
 *         ESP_ERR_NOT_FOUND if no sidecar file exists
 *         ESP_ERR_INVALID_ARG if meta is NULL or filepath not set
 *         ESP_ERR_INVALID_STATE if JSON parsing fails
 */
esp_err_t animation_metadata_load_sidecar(animation_metadata_t *meta);

/**
 * @brief Check if metadata has a valid filepath
 * 
 * @param meta Metadata structure
 * @return true if filepath is set and non-empty
 */
bool animation_metadata_has_filepath(const animation_metadata_t *meta);

/**
 * @brief Get the metadata filepath
 * 
 * @param meta Metadata structure
 * @return Filepath string or NULL if not set
 */
const char *animation_metadata_get_filepath(const animation_metadata_t *meta);

/**
 * @brief Copy metadata from source to destination
 * 
 * Deep copies all strings. Destination should be initialized or freed first.
 * 
 * @param dst Destination metadata structure
 * @param src Source metadata structure
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t animation_metadata_copy(animation_metadata_t *dst, const animation_metadata_t *src);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_METADATA_H

