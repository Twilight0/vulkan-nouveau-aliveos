# vulkan-nouveau-aliveos TODO (`nvk-fermi` Roadmap)

Target hardware: NVIDIA GeForce GT 555M (`GF106M`, `NVC3`, Fermi `SM21`, `FERMI_A`/`B` 3D `0x9097`/`0x9197`, `FERMI_COMPUTE_A` `0x90c0`, `PCOPY` `0x90b5`).

---

## 1. Codebase Research Findings (Mesa `26.3.1` / `27.0.0-devel` & `nouveau-dkms`)

### What Is Already Present Upstream
1. **Collabora's `NAK` compiler already has an `SM20` backend (`src/nouveau/compiler/nak/sm20.rs`, 3,155 lines)**:
   - `ShaderModel20::new(sm)` accepts `sm >= 20 && sm < 32`, defines `RegFile::GPR => 63`, `RegFile::Pred => 7`, `RegFile::Carry => 1`.
   - `OpTex::encode()` (`sm20.rs:1745-1758`) **already implements Fermi bound texture indices**:
     - `TexRef::Bound(idx)` -> `e.set_field(32..40, idx); e.set_bit(50, false);`
     - `TexRef::Bindless` -> gated behind `assert!(e.sm.sm() >= 30);`
   - Currently gated at `sm20.rs:3153` with `assert!(sm.sm >= 30);` because Collabora brought up Kepler (`SM30`) first.
2. **`NVK` (`src/nouveau/vulkan/`) already supports Kepler (`KEPLER_A`, `0xa097`, `SM30`)**:
   - Hard device gate is at `nvk_physical_device.c:1561` (`if (nvkmd->dev_info.cls_eng3d < KEPLER_A)`).
   - Fermi `0x9097` and Kepler `0xa097` share ~85%+ of 3D rasterizer, blend, depth/stencil, viewport, vertex attribute, MME macro, and copy engine (`0x90b5`) command sets.
3. **Kernel prerequisites (`nouveau-dkms`)**:
   - `nouveau_drm.c` / `nouveau_uvmm.c` already expose `DRM_IOCTL_NOUVEAU_VM_BIND` and `EXEC` syncobj uAPI required by `NVK`.
   - Full Fermi core/shader (`590/1180 MHz`, `700/1400 MHz`) and DDR3 (`900 MHz`) reclocking is already working in `nouveau-dkms`.

### Core Architectural Gaps to Bridge for Fermi (`SM20`/`SM21`)
1. **NAK `SM20` Shader Emission (`sm20.rs` & `sm30_instr_latencies.rs`)**:
   - Kepler (`SM30`) injects an explicit software-scheduling control word every 7 instructions (`encode_kepler_shader`); Fermi (`SM20`/`SM21`) uses hardware scoreboarding and packs 8 instructions per 64-byte block with no `sched` control word.
   - Needs a `sm < 30` emitter path plus minor `SM20` vs `SM30` opcode/legalization differences (reference: `src/gallium/drivers/nouveau/codegen/nv50_ir_emit_nvc0.cpp`).
2. **Bound-Descriptor Lowering (`hasvk`-style)**:
   - **UBOs & SSBOs**: Fermi supports native 64-bit global pointers (`LD.E` / `ST.E` / `ATOM.E`), so NVK's descriptor-buffer address model works almost unchanged for buffer descriptors.
   - **Sampled Images & Samplers**: Fermi `TEX` encodes bound `TIC` (`0..127`) and `TSC` (`0..31`) slots as instruction immediates (`TexRef::Bound(idx)`) rather than Kepler's bindless GPR handles. Needs a NIR lowering pass mapping `(set, binding, array_index)` to deterministic `TIC`/`TSC` slots and emitting `NVC0_3D_BIND_TIC` / `NVC0_3D_BIND_TSC` on `vkCmdBindDescriptorSets` / draw, plus a waterfall loop for dynamic array indexing.
   - **Storage Images**: Map to Fermi's 8 hardware `SU` surface slots (`SULD`/`SUST`/`SURED`).
3. **Fermi Compute Class `0x90c0` (`nvk_cmd_dispatch.c`)**:
   - Kepler+ uses `QMD` structs (`0xa0c0`), whereas Fermi uses `FERMI_COMPUTE_A` (`0x90c0`) method-based launches (reference: `src/gallium/drivers/nouveau/nvc0/nvc0_compute.c`).
4. **Fermi 3D / Copy Differences & Vulkan 1.0/1.1 Feature Gating (`nvk_physical_device.c`)**:
   - Adapt `0x9097` vs `0xa097` state/shader header (`SPH`) and `0x90b5` copy methods.
   - Gate Kepler+/Maxwell+ extensions (`EXT_descriptor_buffer`, `KHR_shader_atomic_int64`, `sparseBinding`, `KHR_vulkan_memory_model`, `shaderStorageImageReadWithoutFormat`, etc.) to target compliant **Vulkan 1.0 / 1.1**.

---

## 2. Milestones

### Milestone 1: Bring-Up (`vulkaninfo` + `vkcube`)
- [x] Verify `nouveau-dkms` `VM_BIND` + `EXEC` syncobj ioctls with `NVK` winsys on `GF106M` (`NVC3`).
- [x] Lower `nvk_physical_device.c` chipset gate from `KEPLER_A` to `FERMI_A` (guarded behind `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1` / `nvk-fermi` patch) and gate unsupported Kepler+ init/copy methods.
- [x] Verify `SM21` (`encode_sm20_shader`) shader emission compiles and executes vertex + fragment shaders cleanly on `0x9097`.
- [x] Adapt `0x9097` 3D state setup (`SPH` / `NV9039` M2MF copy) so `vulkaninfo` enumerates `GF106M` and `vkcube --wsi wayland` renders cleanly (**101.8 FPS**, zero kernel faults).

### Milestone 2: Bound Descriptors & `0x90c0` Compute
- [x] Implement NIR bound-descriptor lowering pass mapping constant-index `VkDescriptorSet` sampled images/samplers to Fermi `TIC`/`TSC` slots (`TexRef::Bound(idx)` via `nvk_bound_tex_map`).
- [x] Emit `BIND_TIC` / `BIND_TSC` updates in `nvk_cmd_draw.c` / `nvk_cmd_dispatch.c` via `nvk_cmd_buffer_emit_bound_tex()`.
- [x] Handle dynamic/non-constant descriptor array indexing (waterfall loop) and `txs`/`txq` bound-texture queries (`emit_fermi_tex_waterfall` + `NAK_NIR_TEX_REF_TYPE_BOUND` in `build_txq`).
- [x] Implement `FERMI_COMPUTE_A` (`0x90c0`) dispatch path in `nvk_cmd_dispatch.c` (init, `nvk_cmd_dispatch_fermi`, and `NVK_MME_SELECT_CB0` restoration for 3D/compute interop).
- [x] Implement 8-slot `SU` storage image binding (`SULD`/`SUST` via `nvk_bound_su_map`, `nvk_cmd_buffer_emit_bound_su`, and `SM20` `OpSuLd`/`OpSuSt`).
- [x] Add `testing/vk_smoke.c` hardware regression suite covering linear/optimal draws, `NV9039` (`M2MF`) copies, bound textures, `0x90c0` compute SSBO, dynamic texture array + `textureSize()`, and `SU` storage image `imageStore` (6/6 passing, zero `dmesg` faults).
- [~] Verify with textured Vulkan 1.0/1.1 workloads: `vkcube --wsi wayland` (**102.6 FPS**, zero faults); `mpv --vo=gpu-next --gpu-api=vulkan` currently hits `HUB/PFIFO PAGE_NOT_PRESENT` during `libplacebo` slab/query-pool init (`vkmark`, PPSSPP/RetroArch, Dolphin remaining).

### Milestone 3: `DXVK 1.10.3`, `Zink` & Packaging
- [~] Run targeted Vulkan 1.0/1.1 regression & CTS-like subsets (`deqp-vk` not installed; `testing/vk_smoke.c` expanded to **8/8 passing** including `graphics_indexed_ubo` + `compute_image_rw` `SULD.B`/`SUST.P`, fixed `NV90C0` `CRS_SIZE=0`, `NV90C0_SET_SHADER_LOCAL_MEMORY_E`, `NV9097_BIND_GROUP_CONSTANT_BUFFER(0..4)` + `NV9097_INVALIDATE_SHADER_CACHES` for 3D-after-compute, and `OpSuLd` format-based byte shift).
- [x] Test **DXVK 1.10.3** (Vulkan 1.1 / DirectX 9–11) on Fermi: `wine 11.22` + `dxvk-1.10-bin` (`dxdiag` + `testing/d3d11_smoke.c`: `D3D11CreateDevice` FL `11_0`, `ClearRenderTargetView`, `Draw` `vs_4_0`/`ps_4_0`, and `Dispatch` `cs_5_0` `RWStructuredBuffer` UAV) all pass (**exit=0**, zero `nouveau` kernel faults) after adding `fermi_su` support to `nvk_buffer_view` (`VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER`).
- [~] Test **Mesa Zink** (OpenGL 4.6 over `nvk-fermi`): `glxgears` on Zink progressed past `TEMP_TOO_SMALL` and still hits `PGRAPH` `READ at 0x0` (`VK_ERROR_DEVICE_LOST`); needs further Zink descriptor/WSI investigation.
- [x] Lock exact crate & patch `sha256sums` in `PKGBUILD` and generate `.SRCINFO` (no `SKIP` entries remain; ready to publish `Twilight0/vulkan-nouveau-aliveos` to GitHub + AUR + `aliveos-repo`).
