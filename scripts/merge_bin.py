# PlatformIO post-build: write firmware-merged.bin (bootloader + partitions +
# boot_app0 + app) so a blank ESP32 can be flashed at 0x0 in one shot.
# ponytail: leans on espressif32 builder internals (FLASH_EXTRA_IMAGES,
# ESP32_APP_OFFSET, __get_board_*); platform is pinned in platformio.ini and
# every lookup below fails the build loudly if a platform bump renames them.
Import("env")  # noqa: F821


def merge_bin(source, target, env):
    extra = env.get("FLASH_EXTRA_IMAGES", [])
    app_offset = env.subst("$ESP32_APP_OFFSET")
    flash_mode = env.subst("${__get_board_flash_mode(__env__)}")
    flash_freq = env.subst("${__get_board_f_flash(__env__)}")
    # A merged image without bootloader/partitions bricks a blank chip at 0x0.
    if not extra or not app_offset.startswith("0x") or not flash_mode or not flash_freq:
        raise SystemExit("merge_bin: espressif32 builder internals missing; fix scripts/merge_bin.py")
    build = env.subst("$BUILD_DIR")
    images = [x for img in extra for x in (img[0], env.subst(img[1]))]
    images += [app_offset, f"{build}/firmware.bin"]
    if env.Execute(" ".join([
        '"$PYTHONEXE"', '"$OBJCOPY"', "--chip", env.BoardConfig().get("build.mcu"),
        "merge_bin", "-o", f'"{build}/firmware-merged.bin"',
        "--flash_mode", flash_mode,
        "--flash_freq", flash_freq,
        "--flash_size", env.BoardConfig().get("upload.flash_size", "4MB"),
        *images,
    ])):
        raise SystemExit("merge_bin: esptool merge_bin failed")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)  # noqa: F821
