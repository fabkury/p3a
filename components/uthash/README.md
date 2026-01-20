# uthash - Hash Tables for C

This component provides the uthash header-only library for hash table support in C.

## About uthash

uthash is a header-only hash table implementation for C structures.

- **Source**: https://github.com/troydhanson/uthash
- **License**: BSD (compatible with Apache-2.0)
- **Version**: master branch (latest)

## Usage

Include the header in your code:

```c
#include "uthash.h"
```

uthash provides macros for adding hash table functionality to any C structure.
See the official documentation for detailed usage: https://troydhanson.github.io/uthash/

## Why uthash?

uthash was chosen for p3a because:
- Header-only (no linking required)
- Zero dependencies
- Well-tested and widely used
- Efficient O(1) average-case lookups
- Supports multiple key types (int, string, pointer, etc.)
- Works well with ESP-IDF's memory model
