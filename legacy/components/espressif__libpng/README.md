# espressif__libpng (local fork with APNG read support)

This is a **local fork** of the `espressif/libpng` managed component, kept under
`components/` so it overrides the registry version by name (`REQUIRES
espressif__libpng` elsewhere in the project resolves here unchanged).

## Provenance

- Upstream component: `espressif/libpng` v1.6.52 from
  [idf-extra-components](https://github.com/espressif/idf-extra-components/tree/master/libpng),
  commit `8e8bebda0b2c5d00d2a98ed9ec2f0afe7a30953f` (bundles vanilla libpng 1.6.52
  in `libpng/`, pre-generated `pnglibconf.h` at the component root).
- Applied on top: the official APNG patch for **exactly** libpng 1.6.52 —
  `libpng-1.6.52-apng.patch` (committed in this directory for provenance), from
  the [libpng-apng project](https://sourceforge.net/projects/libpng-apng/)
  (same patch lineage Firefox ships). It applied cleanly to all 13 files.
- Two local changes **on top of the patch** (re-apply both when re-patching):
  1. `libpng/png.h`: the patch's unconditional `#define PNG_WRITE_APNG_SUPPORTED`
     is removed — p3a is decode-only, so only `PNG_APNG_SUPPORTED` +
     `PNG_READ_APNG_SUPPORTED` are defined. Write-side APNG code
     (`pngwrite.c`/`pngwutil.c` hunks, `png_write_frame_head/tail`) compiles out.
  2. `libpng/pngtest.c`: `#undef pass_height` added after the patch's APNG frame
     loop — the patch leaves the macro defined, which collides with the identical
     `#define` in the stock non-APNG loop ("pass_height redefined" warning).

## Why a fork

Stock libpng 1.6.x treats the APNG chunks (`acTL`/`fcTL`/`fdAT`) as unknown and
skips them, so APNG files render as a static first frame. Upstream libpng merged
APNG into its development branch (future 2.0) in June 2025, but no tagged
release has it and the Espressif component tracks 1.6.x. The fork is the only
way to ship APNG decode today.

## Maintenance obligation (re-patch on every libpng bump)

When Espressif bumps their libpng component (e.g., for a CVE):

1. Fetch the new component source (e.g., temporarily re-add `espressif/libpng`
   to `main/idf_component.yml`, build once, copy from `managed_components/`,
   then remove the dependency again).
2. Download the **matching** `libpng-1.6.x-apng.patch.gz` from
   <https://sourceforge.net/projects/libpng-apng/files/libpng16/> — the project
   republishes the patch for every libpng release.
3. Apply with `patch -p1` inside `libpng/`, re-remove
   `PNG_WRITE_APNG_SUPPORTED` from `png.h` (see above), and replace the
   committed `.patch` file with the new version.
4. Keep this component's root `pnglibconf.h`, `CMakeLists.txt`, and
   `idf_component.yml` (they are ours, not upstream's).

The fork can be **retired** (back to the plain managed component) once a tagged
libpng release with native APNG support ships and Espressif packages it
(APNG was merged for libpng 2.0; track
<https://github.com/pnggroup/libpng/pull/706>).

Consumer of the APNG API: `components/animation_decoder/png_animation_decoder.c`.
