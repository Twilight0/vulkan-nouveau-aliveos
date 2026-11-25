# vulkan-nouveau-aliveos

AliveOS patched `vulkan-nouveau` (Mesa NVK + NAK) package targeting **Vulkan 1.0/1.1 support on NVIDIA Fermi (`GF100`–`GF119`, `SM20`/`SM21`)**, structured cleanly on top of official Mesa `26.3.1` tarballs and matching Arch Linux's `vulkan-nouveau` (`1:26.3.1-3`) version for easy rebasing.

Designed as a standalone companion to [`mesa-aliveos`](https://github.com/Twilight0/mesa-aliveos) and [`nouveau-dkms`](https://github.com/Twilight0/nouveau-dkms), keeping Rust and `rust-bindgen` build dependencies isolated from `mesa-aliveos` with zero file overlap.

## What Is Implemented

- **NAK `SM20`/`SM21` Shader Emission (`0001`):**
  - Fermi `SM20`/`SM21` instruction encoding (`encode_sm20_shader`), 8-instruction 64-byte block packing without Kepler software scheduling words, and `SM20` legalization/lowering fixes.
- **NVK `0x9097` 3D & `0x9039` M2MF Copy (`0001`–`0002`):**
  - Lowers the `nvk_physical_device` chipset gate to `FERMI_A` (guarded by `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`), adapts `0x9097` 3D state and `SPH`, and implements `NV9039` (`M2MF`) buffer/image copies.
- **Bound Descriptors & `0x90c0` Fermi Compute (`0002`–`0003`):**
  - Bound `TIC`/`TSC` descriptor lowering (`nvk_bound_tex_map`), `BIND_TIC`/`BIND_TSC` emission, dynamic descriptor array waterfall loops (`emit_fermi_tex_waterfall`), and `txs`/`txq` support.
  - `FERMI_COMPUTE_A` (`0x90c0`) compute dispatch (`nvk_cmd_dispatch_fermi`), `NV90C0_SET_SHADER_LOCAL_MEMORY_E` + `CRS_SIZE` handling, and 3D-after-compute constant buffer / shader cache restoration.
  - 8-slot hardware `SU` surface binding (`SULD`/`SUST`, `nvk_bound_su_map`) for both storage images and `VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER` (`nvk_buffer_view`).

## Status & Verification

Tested on NVIDIA GeForce GT 555M (`GF106M` / `NVC3`, `SM21`) with `nouveau-dkms` and `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`:

- **Native Vulkan (`testing/vk_smoke`):** 8/8 tests passing (linear/optimal draws, `NV9039` copy, textured draw, indexed UBO draw after compute, `0x90c0` compute SSBO, dynamic texture array + `textureSize()`, storage image `imageStore` and `imageLoad`+`imageStore`), zero `nouveau` kernel faults.
- **DXVK 1.10.3 (`testing/d3d11_smoke.exe` under Wine 11.22):** `D3D11CreateDevice` (`FL 11_0`), `ClearRenderTargetView`, `Draw` (`vs_4_0` + `ps_4_0`), and `Dispatch` (`cs_5_0` `RWStructuredBuffer` UAV) all passing, zero `nouveau` kernel faults.
- **`vkcube --wsi wayland`:** Renders cleanly at ~102 FPS, zero kernel faults.
- **Known Work in Progress:** Mesa Zink (`glxgears`) and `mpv --vo=gpu-next --gpu-api=vulkan` still hit GPU faults under investigation.

## Building & Installing

```bash
makepkg -si
```

Runtime usage on Fermi requires opting in to the non-conformant driver flag:

```bash
NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vulkaninfo --summary
```

## Test Suite (`testing/`)

The `testing/` directory contains standalone regression suites for both native Vulkan and DXVK 1.10.3:

```bash
cd testing
make vk_smoke
NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./vk_smoke
```

To build and run the D3D11 DXVK 1.10.3 test (requires `mingw-w64-gcc`, `wine`, and `dxvk-1.10-bin`):

```bash
cd testing
make d3d11_smoke.exe
NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 WINEDLLOVERRIDES="dxgi,d3d9,d3d10core,d3d11=n" wine ./d3d11_smoke.exe
```

## Companion Projects

- [`nouveau-dkms`](https://github.com/Twilight0/nouveau-dkms) — out-of-tree Nouveau kernel driver with Fermi reclocking, VBlank sync, and `VM_BIND` / `EXEC` syncobj uAPI
- [`mesa-aliveos`](https://github.com/Twilight0/mesa-aliveos) — Gallium OpenGL and VA-API drivers with Nouveau VP4/Fermi fixes
