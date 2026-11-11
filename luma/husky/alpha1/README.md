# Pixel 8 Pro (husky) kernel, alpha1

`config-7.3.0-rc5-zuma-luma-husky` is the complete `.config` for the Pixel 8
Pro, built with `ARCH=arm64 LLVM=1 LOCALVERSION=`. The device tree
(`exynos/google/zuma-husky`) is built into the kernel
(`CONFIG_LUMA_EMBEDDED_DTB`) because ABL's own DTB carries no initrd range;
the image builder supplies `CONFIG_INITRAMFS_SOURCE`.

Google's firmware for Wi-Fi (BCM4398), Bluetooth, the AoC, the CS35L41
speaker amplifiers and the CS40L26 haptics is not redistributable. It is
read from the phone's own vendor partition and is never part of this tree.

`../tools/ect-decode.py` and `../tools/fvmap-decode.py` regenerate the CPU
OPP tables in `zuma.dtsi` from the bootloader's ECT and ACPM's fvmap.
