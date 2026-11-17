# Host unit tests

Tier T1 of `docs/2.0/TESTING.md`: p3a logic compiled with the host compiler
and run without a device.

```powershell
cmake -S tests/host -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

On Windows this uses MinGW gcc and runs without sanitizers. On Linux (CI,
or WSL on the workstation) AddressSanitizer and UBSan are on by default:

```bash
wsl -e bash -lc "cd /mnt/c/<repo> && cmake -S tests/host -B /tmp/p3a-host -G Ninja && cmake --build /tmp/p3a-host && ctest --test-dir /tmp/p3a-host --output-on-failure"
```

## Conventions

- One test file per unit, in a folder named after the layer (`util/`,
  `storage/`, ...). One executable and one CTest entry per file, added with
  `p3a_host_test()` in `CMakeLists.txt`.
- The sources under test are compiled straight from `components/`. Only
  code with no ESP-IDF includes can be tested here. When a unit needs a
  FreeRTOS or IDF type, add the smallest possible stand-in under `stubs/`
  (the folder is created with the first one) rather than pulling in more of
  the IDF.
- Warnings are errors: `-Wall -Wextra -Werror -Wshadow -Wconversion`.
- Fixtures (recorded API responses, sample files) go in `fixtures/` and
  never contain an API key.
- A bug found on the device in host-testable code gets a test here that
  fails before the fix.

`third_party/unity/` is Unity 2.x, copied unmodified from ESP-IDF v5.5.4
(`components/unity/unity`), MIT licensed.
