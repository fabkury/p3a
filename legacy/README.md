# legacy/

The p3a 1.x sources as they stood on `main` at `4c9de368`, moved here on the
`v2` branch at milestone M0.1. This folder is the quarry p3a 2.0 is built
from. It is not a project: nothing here is compiled, and nothing in the 2.0
tree may include or link it.

Rules (see `docs/2.0/README.md` and `AGENTS.md`):

- A file leaves this folder only by being moved into the 2.0 tree inside a
  milestone, or by being deleted with a line in the deletion log of
  `docs/2.0/PROGRESS.md`.
- Do not fix or improve code in place here. A 1.x fix belongs on `main`.
- When this folder is empty, it is removed, and that is a release gate for
  2.0 (milestone M6.2).

What is left (2026-11-25): the Makapix client (`components/makapix`, the
`makapix_channel_*` files of `channel_manager`, and `p3a_core`'s current
post and reaction dispatcher), which milestone M4.5 part 4 still reads,
and:

| File | What |
|---|---|
| `sdkconfig` | The full 1.x configuration as shipped. `tools/compare_sdkconfig.py` checks every 2.0 release build against it |
| `sdkconfig.defaults` | The same configuration reduced to its departures from the IDF defaults (`idf.py save-defconfig`). The 2.0 `sdkconfig.defaults` was seeded from it |
| `managed_components/` | Not tracked. The downloaded 1.x dependencies, kept locally for reference |

Already deleted and never to be moved: PICO-8, the dead code listed in the
architecture review (finding 23), the three forwarding services, and the web
UI over-the-air update stack. The full list is in the ledger.
