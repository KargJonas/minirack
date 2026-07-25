#!/usr/bin/env python3
"""Amalgamate the EasyOTA library into a single drop-in header.

The multi-file library under lib/EasyOTA/src/ is canonical: it carries
library.json, so PlatformIO resolves the ESPAsyncWebServer / AsyncTCP deps
automatically. This script folds those sources into single-header/EasyOTA.h
for Arduino-IDE and "grab one file" users, following the stb pattern:
declarations are always visible; the implementation compiles only in the one
translation unit that does `#define EASYOTA_IMPLEMENTATION` before including.

The generated header is a build artifact - never edit it by hand. Edit the
sources under lib/EasyOTA/src/ and rerun this script.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "lib" / "EasyOTA" / "src"
OUT = ROOT / "single-header" / "EasyOTA.h"

# The .cpp files each open with `#include "EasyOTA.h"`; in the amalgamation
# that's the file including itself, so drop it.
_SELF_INCLUDE = re.compile(r'^#include\s+"EasyOTA\.h"\s*\n', re.MULTILINE)


def read(name: str) -> str:
    return (SRC / name).read_text()


def strip_self_include(text: str) -> str:
    return _SELF_INCLUDE.sub("", text)


def strip_pragma_once(text: str) -> str:
    # The amalgamated file carries its own #pragma once at the very top;
    # remove the one the header source declares so it doesn't recur mid-file.
    return re.sub(r'^#pragma once\s*\n', "", text, count=1, flags=re.MULTILINE)


BANNER = """\
#pragma once
/*
 * EasyOTA - single-header build. GENERATED from lib/EasyOTA/; do NOT edit.
 * Regenerate with:  python3 tools/amalgamate.py
 *
 * Drop-in usage (Arduino IDE, or any project without a library manager):
 *   In exactly ONE .cpp/.ino translation unit, define the implementation:
 *       #define EASYOTA_IMPLEMENTATION
 *       #include "EasyOTA.h"
 *   Include the header the plain way everywhere else.
 *
 * A single header can't inline EasyOTA's dependencies, so you still need:
 *   - the ESPAsyncWebServer and AsyncTCP libraries installed, and
 *   - one interface flag: -DEASYOTA_USE_ETH (WT32-ETH01) or -DEASYOTA_USE_WIFI
 *     (for WiFi, also -DEASYOTA_WIFI_SSID / -DEASYOTA_WIFI_PASS).
 * Arduino IDE has the framework libs (ESPmDNS/Update/Preferences/Ethernet) on
 * the path already; nothing else to do. Under PlatformIO the impl includes sit
 * behind the guard where LDF can't see them, so name them explicitly:
 *   lib_deps =
 *       esp32async/AsyncTCP
 *       esp32async/ESPAsyncWebServer
 *       ESPmDNS
 *       Update
 *       Preferences
 *       Ethernet            ; only for -DEASYOTA_USE_ETH; WiFi builds omit it
 * PlatformIO users are usually better served by the multi-file library (its
 * library.json declares the deps automatically); this file is for the drop-in
 * case. See README.
 */
"""


def main() -> int:
    decl = strip_pragma_once(read("EasyOTA.h"))
    impl_ota = strip_self_include(read("EasyOTA.cpp"))
    impl_net = strip_self_include(read("EasyNet.cpp"))

    parts = [
        BANNER,
        "\n/* ===================== interface (always visible) ==================== */\n\n",
        decl,
        "\n#ifdef EASYOTA_IMPLEMENTATION\n",
        "\n/* ------------------------- EasyOTA.cpp ------------------------------- */\n\n",
        impl_ota,
        "\n/* ------------------------- EasyNet.cpp ------------------------------- */\n\n",
        impl_net,
        "\n#endif /* EASYOTA_IMPLEMENTATION */\n",
    ]

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("".join(parts))
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
