# vulkan-nouveau-aliveos

AliveOS patched `vulkan-nouveau` (Mesa NVK + NAK) package scaffold, structured CachyOS-style on top of official Mesa tarballs.

Designed as a standalone companion to [`mesa-aliveos`](https://github.com/Twilight0/mesa-aliveos) and [`nouveau-dkms`](https://github.com/Twilight0/nouveau-dkms), targeting **Vulkan 1.0/1.1 support on NVIDIA Fermi (`GF100`–`GF119`, `SM20`/`SM21`)** while keeping Rust / `rust-bindgen` build dependencies isolated from `mesa-aliveos`.

## Status

Initial packaging scaffold against upstream Mesa `26.3.1` (no driver patches applied yet). See [TODO.md](TODO.md) for the `nvk-fermi` architecture notes and milestone roadmap.

## Companion Projects

- [`nouveau-dkms`](https://github.com/Twilight0/nouveau-dkms) — out-of-tree Nouveau kernel driver with Fermi reclocking, VBlank sync, and `VM_BIND` / `EXEC` syncobj uAPI
- [`mesa-aliveos`](https://github.com/Twilight0/mesa-aliveos) — Gallium OpenGL and VA-API drivers with Nouveau VP4/Fermi fixes
