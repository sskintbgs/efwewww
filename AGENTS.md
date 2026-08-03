# AGENTS.md

## Cursor Cloud specific instructions

This is a **Windows-only C++** project (XInput / ViGEm / HidHide / raw HID). It
cannot run on the Linux cloud VM, but you can still get real signal:

- **Verify with `./scripts/run_tests.sh`.** It cross-compiles with MinGW-w64,
  compile-checks the whole app (`src/main.cpp`, which transitively includes every
  header), builds the pure parse/convert unit tests, and runs them under Wine.
  Use this as the primary check after touching parsing/conversion/mapping code.
- Toolchain needed on the VM: `g++-mingw-w64-x86-64` and `wine` (installed by the
  environment update script). ViGEmClient is **vendored** under
  `third_party/ViGEmClient`, so there is nothing to fetch.
- **Do not expect the full app to link or run here.** The virtual controller
  needs the ViGEmBus/HidHide kernel drivers (Windows only), and MinGW's HID
  import libraries mangle `HidD_*/HidP_*` names so the exe won't cross-link.
  End-to-end controller testing requires real Windows + ViGEmBus + a controller.
  Keep logic that must be validated inside the pure, testable helpers
  (`report_builders.h`, and the static `ParseSonyReport`/`GetSonyOffsets` in
  `ds4_hid_reader.h`) and add cases to `tests/parse_tests.cpp`.
- Include roots when compiling by hand: the **repo root** (for the vendored
  `third_party/ViGEmClient/include/...` path used by `vigem_loader.h`), `src/`,
  and `third_party/ViGEmClient/include` (for the `ViGEm/Common.h` short include).
- HID report layouts and HidHide IOCTL codes are byte-exact contracts — if you
  change them, cite an authoritative source (nondebug/dualsense, dsremap, the
  Linux `hid-playstation` driver, or nefarius/HidHide's `HidHideIoctlContract.h`).
  DS4 and DualSense do **not** share the same USB byte layout.
- The `#pragma comment(lib, ...)` / cast-function-type / missing-field-initializer
  warnings from MinGW are expected (MSVC-specific pragmas etc.) and are not errors.
