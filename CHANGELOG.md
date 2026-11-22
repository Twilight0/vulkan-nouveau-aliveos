# Changelog

All notable changes to **vulkan-nouveau-aliveos** will be documented in this file.

---

## [Unreleased]

### Initial Scaffold
- Created standalone CachyOS-style packaging repository targeting Mesa `26.3.1` (`-Dvulkan-drivers=nouveau`, `-Dgallium-drivers=`) installing `libvulkan_nouveau.so`, `nouveau_icd.json`, and `license.rst` with zero file overlap against `mesa-aliveos` or `extra/mesa`.
- Added `TODO.md` documenting Mesa `NAK` `sm20.rs` and `NVK` findings and the 3-milestone Fermi Vulkan (`nvk-fermi`) roadmap.
