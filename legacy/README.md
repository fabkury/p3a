# legacy/

What is left of the p3a 1.x sources that were moved here on the `v2` branch
at milestone M0.1 (from `main` at `4c9de368`) as the quarry p3a 2.0 was
built from. Every source file has since been moved into the 2.0 tree or
deleted with a line in the deletion log of `docs/2.0/PROGRESS.md`; the last
of them, the 1.x Makapix client, went on 2026-11-28. 1.x's sources are on
`main`.

Nothing here is compiled, and nothing in the 2.0 tree may include or link
it.

| File | What |
|---|---|
| `sdkconfig` | The full 1.x configuration as shipped. `tools/compare_sdkconfig.py` checks every 2.0 release build against it |
| `sdkconfig.defaults` | The same configuration reduced to its departures from the IDF defaults (`idf.py save-defconfig`). The 2.0 `sdkconfig.defaults` was seeded from it |

The folder goes when the configuration check does, at the release
(milestone M6.4; see O24 in the ledger).

Not tracked, and only on the development workstation:
`managed_components/` (the downloaded 1.x dependencies) and
`components/makapix/certs/` (a copy of the certificate includes a build of
`main` needs, which `main` does not track either).
