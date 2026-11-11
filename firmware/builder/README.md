# Firmware Build Environment (Linux x86_64 / Docker)

JieLi's `pi32v2` C/LTO compiler (`clang` 4.0.1 fork) and post-build packaging tools are distributed as `x86_64` Linux binaries (plus 32-bit Windows `fw_add.exe` / `ufw_maker.exe` from SDK v1.0.0 for stock SincoOTA compatibility).

This directory provides a self-contained **Docker (`linux/amd64`)** build container and standalone scripts that work on any `x86_64` Debian/Ubuntu host (or on Apple Silicon / `arm64` via `docker --platform linux/amd64`).

## Contents

| File | Purpose |
|---|---|
| [Dockerfile](Dockerfile) | `debian:bookworm-slim` (`linux/amd64`) image with `gcc`, `make`, `python3`, 32-bit `wine`, headless Qt/X11 libs for `isd_download`, and USB/SCSI utilities (`sg3-utils`, `usbutils`) |
| [setup_toolchain.py](setup_toolchain.py) | Downloads and verifies the JieLi Linux toolchain (`sha256`-pinned), Linux post-build tools, `Jieli-Tech/fw-AC63_BT_SDK` (`ac63_bt_sdk_release_v2.4.0`), and upstream `kagaimiq/jl-misctools` into `$SMCPAD_WORK` (default `/workspace`) |
| [prepare_stock.py](prepare_stock.py) | Extracts the bootloader (`uboot.boot`), OTA loader (`ota.bin`), and target `.key` file locally from **your own** downloaded official `SMC-PAD_005.fwsc` |
| [build_ufw.sh](build_ufw.sh) | Compiles the overlaid SDK (`make ac636n_spp_and_le`), extracts ELF sections into `app.bin`, and runs `isd_download` |
| [repack_oldchain.sh](repack_oldchain.sh) | Repackages the compiled `app.bin` together with your stock `uboot.boot` and `ota.bin` into the stock-era `.ufw` layout expected by the factory bootloader |
| [build_return_ufw.sh](build_return_ufw.sh) | Optional helper to build a stock-return `.ufw` with the SDK v2.4.0 updater |

## Quick Start (Docker)

1. **Build the builder image:**
   ```bash
   docker build --platform linux/amd64 -t smcpad-builder firmware/builder
   ```

2. **Initialize the toolchain & SDK volume (one-time):**
   ```bash
   docker run --rm -it --platform linux/amd64 \
     -v "$PWD":/repo \
     -v smcpad-work:/workspace \
     smcpad-builder \
     python3 /repo/firmware/builder/setup_toolchain.py
   ```

3. **Prepare stock components from your own `SMC-PAD_005.fwsc` & build:**
   Place your own downloaded official `SMC-PAD_005.fwsc` in `firmware/original/SMC-PAD_005.fwsc` (gitignored), then run inside the container:
   ```bash
   docker run --rm -it --platform linux/amd64 \
     -v "$PWD":/repo \
     -v smcpad-work:/workspace \
     smcpad-builder bash -lc '
       python3 /repo/firmware/builder/prepare_stock.py /repo/firmware/original/SMC-PAD_005.fwsc &&
       python3 /repo/firmware/sdk_overlay/apply.py &&
       KEYFILE=/workspace/keys/smcpad.key \
         NO_TONE=1 \
         bash /repo/firmware/builder/build_ufw.sh ac636n_spp_and_le /workspace/work/sdk-smcpad &&
       bash /repo/firmware/sdk_overlay/test/check_ram_isr.sh /workspace/work/sdk-smcpad /workspace/out/ac636n_spp_and_le
     '
   ```

4. **Wrap for SincoOTA (`.ufw` → `.fwsc`):**
   If repacking for stock OTA (`repack_oldchain.sh`), wrap the resulting `.ufw` with:
   ```bash
   python3 tools/fw/make_fwsc.py update.ufw firmware/build/SMC-PAD_925.fwsc
   ```

See [firmware/sdk_overlay/README.md](../sdk_overlay/README.md), [docs/build_host.md](../../docs/build_host.md), and [docs/first_flash.md](../../docs/first_flash.md) for flash layout verification and safety checks before flashing.
