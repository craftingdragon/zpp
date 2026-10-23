# Maintainer: craftingDragon7
pkgname=zpp
pkgver=0.1.0
pkgrel=1
pkgdesc="Standalone C++20 Zig preprocessor with @cImport lowering and Qt moc-style CMake shadow build"
arch=('x86_64' 'aarch64')
url="https://github.com/julian/zig-preprocessor"
license=('MIT')
depends=('gcc-libs' 'glibc' 'zig')
makedepends=('cmake' 'gcc')
source=()
sha256sums=()

build() {
    cmake -B "${srcdir}/build" -S "${startdir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DZPP_BUILD_TESTS=ON
    cmake --build "${srcdir}/build"
}

check() {
    ctest --test-dir "${srcdir}/build" --output-on-failure
}

package() {
    DESTDIR="${pkgdir}" cmake --install "${srcdir}/build"
    install -Dm644 "${startdir}/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
