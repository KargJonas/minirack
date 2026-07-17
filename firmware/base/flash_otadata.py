# Serial-flashing the base image also resets otadata (the boot-slot selector
# at 0xe000): a board whose otadata points at ota_1 would otherwise keep
# booting whatever sits there instead of the freshly flashed ota_0. Blank
# (0xFF) otadata makes the bootloader fall back to the first OTA slot.
Import("env")
import os

build_dir = env.subst("$BUILD_DIR")
blank = os.path.join(build_dir, "otadata_blank.bin")
os.makedirs(build_dir, exist_ok=True)
if not os.path.exists(blank):
    with open(blank, "wb") as f:
        f.write(b"\xff" * 0x2000)
env.Append(FLASH_EXTRA_IMAGES=[("0xe000", blank)])
