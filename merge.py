Import("env")
import os

def _find_esptool_py(env):
    # PlatformIO embarque esptool ici
    tool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    if not tool_dir:
        return None
    return os.path.join(tool_dir, "esptool.py")

def merge_bins(source, target, env):
    build_dir = env.subst("$BUILD_DIR")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware   = os.path.join(build_dir, "firmware.bin")
    output     = os.path.join(build_dir, "full_firmware.bin")

    esptool_py = _find_esptool_py(env)
    if not esptool_py or not os.path.isfile(esptool_py):
        print("❌ tool-esptoolpy introuvable. Essaie de rebuild ou ajoute platform_packages = tool-esptoolpy.")
        return 1

    python_exe = env.subst("$PYTHONEXE")

    # Détection MCU -> --chip
    mcu = env.BoardConfig().get("build.mcu", "esp32").lower()
    chip = "esp32"
    if "esp32c3" in mcu: chip = "esp32c3"
    elif "esp32s3" in mcu: chip = "esp32s3"
    elif "esp32s2" in mcu: chip = "esp32s2"

    # Détection taille flash (souvent "4MB", "8MB", ...)
    flash_size = env.BoardConfig().get("upload.flash_size", "4MB")

    cmd = (
        f"\"{python_exe}\" \"{esptool_py}\" --chip {chip} merge_bin -o \"{output}\" "
        f"--flash_mode dio --flash_freq 40m --flash_size {flash_size} "
        f"0x1000 \"{bootloader}\" "
        f"0x8000 \"{partitions}\" "
        f"0x10000 \"{firmware}\""
    )

    print("\n🔧 Merging full firmware...")
    print(cmd)

    rc = env.Execute(cmd)
    if rc != 0:
        print("❌ merge_bin failed")
        return rc

    print("✅ full_firmware.bin generated ->", output)
    return 0

# PROGNAME.bin => firmware.bin pour Arduino/ESP32
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bins)