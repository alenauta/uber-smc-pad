# Reproducible x86_64 Debian build environment for the JieLi AC63 (BR25 / pi32v2) SDK.
# Build with:
#   docker build --platform linux/amd64 -t smcpad-builder firmware/builder
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8

RUN dpkg --add-architecture i386 \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates curl git \
        gcc libc6-dev make xz-utils file procps \
        python3 python3-venv python3-pip \
        python3-yaml python3-crcmod python3-tqdm python3-pycryptodome \
        wine wine32:i386 wine64 \
        usbutils sg3-utils libusb-1.0-0 \
        libsm6 libice6 libxkbcommon0 libxkbcommon-x11-0 libx11-6 libx11-xcb1 libxcb1 \
        libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
        libxcb-shape0 libxcb-xfixes0 libxcb-xinerama0 libxcb-xkb1 libxcb-sync1 libxcb-render0 \
        libxcb-shm0 libxcb-cursor0 libxcb-util1 libfontconfig1 libfreetype6 libdbus-1-3 \
        libglib2.0-0 libgl1 libegl1 libudev1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /repo
