#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#include "storage/cache.h"

static const char *TAG = "test_cache";

// Test helper: compute SHA256 hash
static void test_compute_sha256(const char *input, char *output)
{
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)input, strlen(input));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    for (int i = 0; i < 32; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[64] = '\0';
}

TEST_CASE("Cache SHA256 hash computation", "[cache]")
{
    char hash1[65];
    char hash2[65];
    
    test_compute_sha256("test_url_1", hash1);
    test_compute_sha256("test_url_1", hash2);
    
    // Same input should produce same hash
    TEST_ASSERT_EQUAL_STRING(hash1, hash2);
    
    test_compute_sha256("test_url_2", hash2);
    
    // Different input should produce different hash
    TEST_ASSERT_NOT_EQUAL_STRING(hash1, hash2);
    
    // Hash should be 64 characters
    TEST_ASSERT_EQUAL(64, strlen(hash1));
}

TEST_CASE("Cache entry metadata structure", "[cache]")
{
    storage_cache_entry_t entry;
    
    // Verify structure size is reasonable
    TEST_ASSERT(sizeof(entry) < 2048);  // Should be much smaller
    
    // Test field initialization
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.url_hash, "test_hash", sizeof(entry.url_hash) - 1);
    strncpy(entry.original_url, "https://example.com/image.png", sizeof(entry.original_url) - 1);
    entry.file_size = 1024;
    entry.timestamp = 1234567890;
    entry.access_count = 5;
    
    TEST_ASSERT_EQUAL_STRING("test_hash", entry.url_hash);
    TEST_ASSERT_EQUAL_UINT64(1024, entry.file_size);
    TEST_ASSERT_EQUAL_UINT32(5, entry.access_count);
}

TEST_CASE("Cache statistics structure", "[cache]")
{
    storage_cache_stats_t stats;
    
    memset(&stats, 0, sizeof(stats));
    
    TEST_ASSERT_EQUAL_UINT32(0, stats.total_entries);
    TEST_ASSERT_EQUAL_UINT64(0, stats.total_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, stats.hit_count);
    TEST_ASSERT_EQUAL_UINT32(0, stats.miss_count);
}

