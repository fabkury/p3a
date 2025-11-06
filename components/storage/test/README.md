# Storage component unit tests

This directory contains unit tests for the storage component, specifically focusing on cache functionality.

## Running Tests

Tests can be run using ESP-IDF's test framework:

```bash
cd firmware
idf.py test
```

Or run tests for this component specifically:

```bash
idf.py test -C components/storage/test
```

## Test Coverage

- **Cache hash computation**: Validates SHA256 hashing of URLs
- **Cache entry metadata**: Validates structure and field handling
- **Cache statistics**: Validates statistics tracking

## Notes

- These tests focus on core cache logic and don't require SD card access
- Full integration tests (requiring SD card) should be run on hardware
- LRU eviction and cache hit/miss behavior should be tested with actual filesystem

