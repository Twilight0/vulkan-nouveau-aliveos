# Changelog

All notable changes to **vulkan-nouveau-aliveos** will be documented in this file.

---

## [1:26.3.1-5] - 2026-11-25

### Fixed
- `0005-nouveau-nak-SM20-place-oDepth-immediately-after-colo.patch`:
  - Fix Fermi (`SM20`) fragment shader `oDepth` (`gl_FragDepth`) register placement in `nak/from_nir.rs` so `oDepth` immediately follows the last color output when `writes_sample_mask` is false (Kepler+ `SM30+` pads for `oMask`, Fermi does not). Fixes corrupted depth writes when Dolphin runs with Fast Depth Calculation unchecked.
- CI: Extract matching version notes from `CHANGELOG.md` into GitHub Release bodies.

### Testing
- Expanded `testing/vk_smoke.c` with `m2mf_blocklinear_64x64`, `graphics_bc1`, and `graphics_frag_depth` (all passing).

## [1:26.3.1-4] - 2026-11-25

### Fixed
- `0004-nouveau-nvk-Fermi-NV9039-M2MF-fill-update-and-subcha.patch`:
  - Route `nvk_cmd_fill_memory_ce` and `nvk_CmdUpdateMemoryKHR` through `NV9039` (`M2MF`) on Fermi (`cls_eng3d < KEPLER_A`), fixing `ce0: DISPATCH_ERROR [ILLEGAL_MTHD]` in Dolphin.
  - Handle `SUBC_NV9039` in `nvk_cmd_flush_wait_dep`, `nvk_event`, and `nvk_query_pool` (fixing `Unknown subc` assertion in Dolphin).
  - Emit `WFI` before 3D CB0 state restoration in `nvk_cmd_dispatch_fermi`.

## [1:26.3.1-3] - 2026-11-25

### Added
- `0001-nouveau-nvk-nak-initial-Fermi-NVC3-SM20-bring-up.patch`: Initial Fermi (`NVC3`, `SM20`/`SM21`) NAK shader emission and NVK `0x9097` bring-up.
- `0002-nouveau-nvk-nak-Fermi-bound-textures-and-NV9039-M2MF.patch`: Bound `TIC`/`TSC` descriptor lowering and `NV9039` (`M2MF`) copy engine support.
- `0003-nouveau-nvk-nak-Fermi-compute-dynamic-tex-txs-and-SU.patch`: `0x90c0` Fermi compute dispatch, dynamic texture array waterfall + `txs`/`txq`, 8-slot `SU` storage images and `VkBufferView` (`fermi_su`), and 3D-after-compute cache/CB0 restoration.
- `testing/`: Standalone Vulkan (`vk_smoke.c`) and DXVK 1.10.3 (`d3d11_smoke.c`) regression suites with precompiled SPIR-V shader headers.
- Aligned package version (`epoch=1`, `pkgver=26.3.1`, `pkgrel=3`) with Arch Linux `extra/vulkan-nouveau`.
