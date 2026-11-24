# Maintainer: Twilight0 <twilight0@vivaldi.net>

pkgname=vulkan-nouveau-aliveos
pkgver=26.3.1
pkgrel=1
pkgdesc="Open-source Vulkan driver for NVIDIA GPUs (AliveOS NVK / Fermi research fork)"
url="https://github.com/Twilight0/vulkan-nouveau-aliveos"
arch=('x86_64')
license=('MIT AND BSD-3-Clause AND SGI-B-2.0')
depends=(
  'expat'
  'glibc'
  'libdisplay-info'
  'libdrm'
  'libelf'
  'libgcc'
  'libstdc++'
  'libx11'
  'libxcb'
  'libxshmfence'
  'spirv-tools'
  'systemd-libs'
  'vulkan-icd-loader'
  'vulkan-mesa-implicit-layers'
  'wayland'
  'xcb-util-keysyms'
  'zlib'
  'zstd'
)
makedepends=(
  'bison'
  'cbindgen'
  'clang'
  'cmake'
  'elfutils'
  'flex'
  'glslang'
  'libxrandr'
  'llvm'
  'meson'
  'python-mako'
  'python-packaging'
  'python-yaml'
  'rust'
  'rust-bindgen'
  'spirv-llvm-translator'
  'wayland-protocols'
  'xorgproto'
)
optdepends=('vulkan-mesa-layers: additional vulkan layers')
provides=(
  "vulkan-nouveau=1:${pkgver}-${pkgrel}"
  'vulkan-driver'
)
conflicts=('vulkan-nouveau')
options=(
  !lto
  !debug
)
source=(
  "https://archive.mesa3d.org/mesa-${pkgver}.tar.xz"
  "0001-nouveau-nvk-nak-initial-Fermi-NVC3-SM20-bring-up.patch"
  "0002-nouveau-nvk-nak-Fermi-bound-textures-and-NV9039-M2MF.patch"
)

# Rust crates required by Mesa NVK / NAK subprojects (Mesa 26.3.1)
declare -A _crates=(
  bitflags         2.9.1
  cfg-if           1.0.0
  equivalent       1.0.1
  errno            0.3.12
  hashbrown        0.14.1
  indexmap         2.2.6
  libc             0.2.185
  log              0.4.27
  mach2            0.7.0
  once_cell        1.8.0
  paste            1.0.14
  pest             2.8.0
  pest_derive      2.8.0
  pest_generator   2.8.0
  pest_meta        2.8.0
  proc-macro2      1.0.86
  quote            1.0.35
  remain           0.2.12
  roxmltree        0.20.0
  rustc-hash       2.1.1
  rustix           1.1.4
  syn              2.0.87
  thiserror        2.0.11
  thiserror-impl   2.0.11
  ucd-trie         0.1.6
  unicode-ident    1.0.12
  windows-link     0.2.0
  windows-sys      0.61.1
  xml              1.2.1
  zerocopy         0.8.13
  zerocopy-derive  0.8.13
)

for _crate in "${!_crates[@]}"; do
  _ver="${_crates[$_crate]}"
  source+=(
    "${_crate}-${_ver}.tar.gz::https://static.crates.io/crates/${_crate}/${_ver}/download"
  )
done

sha256sums=('4e4e438eac7b840a9383988cbb7c08dad8041da544b9319fc26d319fa7348961'
            'SKIP'
            'SKIP'
            'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP'
            'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP'
            'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP'
            'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')

prepare() {
  cd "mesa-${pkgver}"

  local src
  for src in "${source[@]}"; do
    src="${src%%::*}"
    src="${src##*/}"
    [[ $src = *.patch ]] || continue
    echo "Applying patch $src..."
    patch -Np1 < "../$src"
  done

  echo "${pkgver}-aliveos.${pkgrel}" > VERSION
}

build() {
  local meson_options=(
    --optimization 2
    -D android-libbacktrace=disabled
    -D b_ndebug=true
    -D egl=disabled
    -D expat=enabled
    -D gallium-drivers=
    -D gallium-mediafoundation=disabled
    -D gallium-rusticl=false
    -D gallium-va=disabled
    -D gbm=disabled
    -D gles1=disabled
    -D gles2=disabled
    -D glvnd=disabled
    -D glx=disabled
    -D html-docs=disabled
    -D intel-rt=disabled
    -D libunwind=disabled
    -D llvm=enabled
    -D microsoft-clc=disabled
    -D platforms=x11,wayland
    -D shared-llvm=enabled
    -D spirv-tools=enabled
    -D teflon=false
    -D valgrind=disabled
    -D vulkan-drivers=nouveau
    -D vulkan-layers=
    -D vulkan-manifest-per-architecture=false
    -D xmlconfig=enabled
    -D zlib=enabled
    -D zstd=enabled
  )

  export MESON_PACKAGE_CACHE_DIR="$srcdir"

  arch-meson "mesa-${pkgver}" build --auto-features=auto "${meson_options[@]}"
  meson compile -C build
}

package() {
  local staging="$srcdir/stage"
  rm -rf "$staging"
  meson install -C build --destdir "$staging" --no-rebuild

  install -Dm755 "$staging/usr/lib/libvulkan_nouveau.so" -t "$pkgdir/usr/lib"
  install -Dm644 "$staging/usr/share/vulkan/icd.d/nouveau_icd.json" -t "$pkgdir/usr/share/vulkan/icd.d"
  install -Dm644 "mesa-${pkgver}/docs/license.rst" -t "$pkgdir/usr/share/licenses/${pkgname}"
}
