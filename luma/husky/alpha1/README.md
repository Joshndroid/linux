# Pixel 8 Pro (husky) kernel, alpha1

`config-7.3.0-rc5-zuma-luma-husky` is the complete `.config` for the Pixel 8
Pro, built with `ARCH=arm64 LLVM=1 LOCALVERSION=`. The device tree
(`exynos/google/zuma-husky`) is built into the kernel
(`CONFIG_LUMA_EMBEDDED_DTB`) because ABL's own DTB carries no initrd range;
the image builder supplies `CONFIG_INITRAMFS_SOURCE`.

The command line is built in and forced (`CONFIG_CMDLINE_FORCE`): ABL's own
command line lives in the DTB the kernel does not use. `CONFIG_CMDLINE` is
SimplyLuma/luma `devices/google/husky/boot.toml` `[cmdline] args` followed by
the generated arguments, `pmos_root_uuid` and `pmos_boot_uuid` of the generic
phone userdata image and `ostree=aboot`; Luma's
`scripts/mobile/husky/build-boot.sh` refuses a config that differs.

Google's firmware for Wi-Fi (BCM4398), Bluetooth, the AoC, the CS35L41
speaker amplifiers and the CS40L26 haptics is not redistributable and is
never part of this tree. SimplyLuma/luma `scripts/mobile/husky/firmware.py`
extracts it from Google's factory image, checked against
`devices/google/husky/firmware.manifest`, and the boot image carries it.

`../tools/ect-decode.py` and `../tools/fvmap-decode.py` regenerate the CPU
OPP tables in `zuma.dtsi` from the bootloader's ECT and ACPM's fvmap.
