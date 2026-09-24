# PlatformIO post-build step: writes prebuilt/wledlink-bridge.bin, a single image that
# `wledlink.py flash-bridge` (or `esptool write-flash 0x0 ...`) puts on a blank ESP32.
import os

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)


def merge_image(source, target, env):
    build = env.subst("$BUILD_DIR")
    platform = env.PioPlatform()
    parts = [
        ("0x1000", os.path.join(build, "bootloader.bin")),
        ("0x8000", os.path.join(build, "partitions.bin")),
        ("0xe000", os.path.join(platform.get_package_dir("framework-arduinoespressif32"), "tools", "partitions", "boot_app0.bin")),
        ("0x10000", os.path.join(build, "firmware.bin")),
    ]
    missing = [path for _, path in parts if not os.path.exists(path)]
    if missing:
        print("merge_image: skipped, missing " + ", ".join(missing))
        return
    out_dir = os.path.join(env.subst("$PROJECT_DIR"), "prebuilt")
    os.makedirs(out_dir, exist_ok=True)
    esptool = os.path.join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")
    args = [env.subst("$PYTHONEXE"), esptool, "--chip", "esp32", "merge_bin",
            "-o", os.path.join(out_dir, "wledlink-bridge.bin"),
            "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"]
    for offset, path in parts:
        args += [offset, path]
    env.Execute(" ".join('"%s"' % a for a in args))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_image)  # noqa: F821
