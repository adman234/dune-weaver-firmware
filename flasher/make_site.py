#!/usr/bin/env python3
"""Assemble the web flasher site (GitHub Pages) from built firmware.

Run after building both boards:

    pio run -e sandtable && pio run -e sandtable -t buildfs
    pio run -e sandtable_s3 && pio run -e sandtable_s3 -t buildfs
    python3 flasher/make_site.py _site

The site is static and same-origin: the page, its script, every .bin image and
the table configs are served from one Pages deployment, so Web Serial flashing
needs no CORS and no third-party download. `fw/manifest.json` lists images
with their offsets and SHA-256, which the page checks before writing.

Offsets must match build-dw-release.py: the S3 bootloader is at 0x0 (not
0x1000) and its littlefs at 0x610000.
"""

import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGETS = {
    "esp32": {
        "env": "sandtable",
        "label": "MKS DLC32 (ESP32)",
        "chip": "ESP32",
        "images": [
            ("bootloader", 0x1000, "bootloader.bin"),
            ("partitions", 0x8000, "partitions.bin"),
            ("bootapp", 0xE000, "boot_app0.bin"),
            ("firmware", 0x10000, "firmware.bin"),
            ("filesystem", 0x3D0000, "littlefs.bin"),
        ],
    },
    "esp32s3": {
        "env": "sandtable_s3",
        "label": "MKS DLC32 MAX (ESP32-S3)",
        "chip": "ESP32-S3",
        "images": [
            ("bootloader", 0x0, "bootloader.bin"),
            ("partitions", 0x8000, "partitions.bin"),
            ("bootapp", 0xE000, "boot_app0.bin"),
            ("firmware", 0x10000, "firmware.bin"),
            ("filesystem", 0x610000, "littlefs.bin"),
        ],
    },
}

# Table profiles: a config.yaml from the repo plus the few values that differ.
# The original Dune Weaver is the Pro mechanism with a smaller rho gear and
# fewer LEDs (same DLC32, same theta gearing).
PROFILES = [
    {"id": "pro", "label": "Dune Weaver Pro", "mcu": "esp32",
     "config": "configs/dune_weaver_pro/config.yaml"},
    {"id": "old", "label": "Dune Weaver (original)", "mcu": "esp32",
     "config": "configs/dune_weaver_pro/config.yaml",
     "ySteps": 287, "leds": 75, "hostname": "DWOld",
     "name": "Dune Weaver ThetaRho"},
    {"id": "mini_pro", "label": "Dune Weaver Mini Pro", "mcu": "esp32",
     "config": "configs/dune_weaver_mini_pro/config.yaml"},
    {"id": "gold", "label": "Dune Weaver Gold", "mcu": "esp32",
     "config": "configs/dune_weaver_gold/config.yaml"},
    {"id": "max", "label": "DLC32 MAX ThetaRho", "mcu": "esp32s3",
     "config": "config_dlc32max_thetarho.yaml"},
]

STATIC = ["index.html", "flasher.js", "xmodem.js"]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=ROOT).decode().strip()
    except Exception:
        return ""


def bootapp():
    path = os.path.join(os.path.expanduser("~"), ".platformio", "packages",
                        "framework-arduinoespressif32", "tools", "partitions",
                        "boot_app0.bin")
    if not os.path.isfile(path):
        sys.exit("Missing %s (build a target with PlatformIO first)" % path)
    return path


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "_site"
    out = os.path.abspath(out)
    if os.path.exists(out):
        shutil.rmtree(out)
    fw = os.path.join(out, "fw")
    os.makedirs(os.path.join(fw, "configs"))

    for name in STATIC:
        shutil.copy(os.path.join(ROOT, "flasher", name), os.path.join(out, name))
    # Pages would otherwise run Jekyll over the site.
    open(os.path.join(out, ".nojekyll"), "w").close()

    manifest = {
        "name": "Dune Weaver Firmware",
        "version": git("describe", "--tags", "--always", "--dirty"),
        "commit": git("rev-parse", "--short", "HEAD"),
        # owner/name on GitHub Actions; the page links its source from this.
        "repo": os.environ.get("GITHUB_REPOSITORY", ""),
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "targets": {},
        "profiles": [],
    }

    for mcu, t in TARGETS.items():
        build = os.path.join(ROOT, ".pio", "build", t["env"])
        os.makedirs(os.path.join(fw, mcu))
        images = {}
        for name, offset, filename in t["images"]:
            src = bootapp() if name == "bootapp" else os.path.join(build, filename)
            if not os.path.isfile(src):
                sys.exit("Missing build artifact: %s" % src)
            rel = "%s/%s" % (mcu, filename)
            shutil.copy(src, os.path.join(fw, rel))
            images[name] = {"offset": offset, "path": rel,
                            "size": os.path.getsize(src), "sha256": sha256(src)}
            print("  %-8s %-10s %8d bytes @ 0x%06x" % (mcu, name, images[name]["size"], offset))
        manifest["targets"][mcu] = {"label": t["label"], "chip": t["chip"], "images": images}

    for p in PROFILES:
        src = os.path.join(ROOT, p["config"])
        if not os.path.isfile(src):
            sys.exit("Missing profile config: %s" % src)
        rel = "configs/%s.yaml" % p["id"]
        shutil.copy(src, os.path.join(fw, rel))
        entry = {k: v for k, v in p.items() if k != "config"}
        entry["config"] = rel
        manifest["profiles"].append(entry)

    with open(os.path.join(fw, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("site written to %s (%s)" % (out, manifest["version"]))


if __name__ == "__main__":
    main()
