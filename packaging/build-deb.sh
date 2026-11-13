#!/bin/bash
# Build a lumen .deb. Runs inside a Debian 13 (trixie) container or machine.
#   packaging/build-deb.sh            -> dist/lumen_<ver>_<arch>.deb
# scenefx 0.2.1 (+ our patch) is built as a static library and linked into
# the lumen binary, so the package needs nothing that isn't in Debian.
set -euo pipefail
SRC=$(cd "$(dirname "$0")/.." && pwd)
VER=$(sed -n "s/.*version: '\([^']*\)'.*/\1/p" "$SRC/meson.build" | head -1)
ARCH=$(dpkg --print-architecture)
SCENEFX_VER=0.2.1
WORK=${WORK:-/tmp/lumen-pkg}
OUT=${OUT:-$SRC/dist}

if [ "${SKIP_DEPS:-0}" != 1 ]; then
  export DEBIAN_FRONTEND=noninteractive
  SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq --no-install-recommends \
    build-essential meson ninja-build pkg-config curl ca-certificates patch \
    libwlroots-0.18-dev wayland-protocols libwayland-dev libxkbcommon-dev \
    libinput-dev libcairo2-dev libpango1.0-dev libdrm-dev libpixman-1-dev \
    libegl-dev libgles-dev dpkg-dev >/dev/null
fi

rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"
cd "$WORK"

echo ">> scenefx $SCENEFX_VER (static)"
curl -fsSL "https://github.com/wlrfx/scenefx/archive/refs/tags/$SCENEFX_VER.tar.gz" | tar xz
cd "scenefx-$SCENEFX_VER"
patch -p1 < "$SRC/patches/scenefx-0.2.1-shadow-dither-nan.patch"
meson setup build --prefix="$WORK/sfx" -Ddefault_library=static -Dexamples=false \
  --buildtype=release >/dev/null
ninja -C build >/dev/null
ninja -C build install >/dev/null
cd "$WORK"

echo ">> lumen $VER"
export PKG_CONFIG_PATH="$WORK/sfx/lib/pkgconfig:$WORK/sfx/lib/$(gcc -dumpmachine)/pkgconfig"
meson setup build "$SRC" --prefix=/usr -Dpanel=true >/dev/null
ninja -C build
DESTDIR="$WORK/root" ninja -C build install >/dev/null
strip --strip-unneeded "$WORK/root/usr/bin/lumen" "$WORK/root/usr/bin/lumen-panel"

install -Dm644 "$SRC/data/lumen.conf" "$WORK/root/usr/share/doc/lumen/examples/lumen.conf"
install -Dm644 "$SRC/LICENSE" "$WORK/root/usr/share/doc/lumen/copyright"

echo ">> package"
mkdir -p "$WORK/debian" "$WORK/root/DEBIAN"
printf 'Source: lumen\n\nPackage: lumen\nArchitecture: any\nDepends: ${shlibs:Depends}\n' > "$WORK/debian/control"
SHLIBS=$(cd "$WORK" && dpkg-shlibdeps -O -e root/usr/bin/lumen -e root/usr/bin/lumen-panel 2>/dev/null \
  | sed -n 's/^shlibs:Depends=//p')
SIZE=$(du -sk "$WORK/root/usr" | cut -f1)
cat > "$WORK/root/DEBIAN/control" <<CTRL
Package: lumen
Version: $VER
Architecture: $ARCH
Maintainer: Lumen developers <lumen@localhost>
Installed-Size: $SIZE
Depends: $SHLIBS, fuzzel, foot | x-terminal-emulator, mako-notifier, swaylock, swayidle, grim
Recommends: slurp, wl-clipboard, libnotify-bin, fonts-inter, brightnessctl, pulseaudio-utils, xdg-desktop-portal-wlr, adwaita-icon-theme, xdg-user-dirs
Section: x11
Priority: optional
Homepage: https://github.com/AnupamKris/lumen
Description: Lumen - a light, good-looking Wayland desktop
 A small wlroots/SceneFX compositor with floating and tiling layouts,
 rounded corners, soft shadows and blur, plus a minimal panel, launcher,
 lock screen and notifications. Installs as a separate session; your
 existing desktop is left untouched.
CTRL
dpkg-deb --root-owner-group -Zxz --build "$WORK/root" "$OUT/lumen_${VER}_${ARCH}.deb" >/dev/null
echo ">> $OUT/lumen_${VER}_${ARCH}.deb"
dpkg-deb -I "$OUT/lumen_${VER}_${ARCH}.deb" | sed -n '/Depends/p'
ls -lh "$OUT/lumen_${VER}_${ARCH}.deb"
