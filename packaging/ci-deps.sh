#!/bin/sh
# 在 CI 容器里安装构建依赖：packaging/ci-deps.sh <deb|rpm|arch>
set -eu
case "$1" in
    deb)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends \
            build-essential pkg-config libglib2.0-dev libpulse-dev libpipewire-0.3-dev \
            libx11-dev libwayland-dev libwayland-bin dpkg-dev ;;
    rpm)
        dnf install -y gcc make pkgconf-pkg-config glib2-devel pulseaudio-libs-devel pipewire-devel \
            libX11-devel wayland-devel \
            rpm-build tar gzip ;;
    arch)
        pacman -Syu --noconfirm --needed base-devel glib2 libpulse libpipewire libx11 wayland pkgconf
        id builder >/dev/null 2>&1 || useradd -m builder ;;
    *) echo "unknown kind: $1" >&2; exit 1 ;;
esac
