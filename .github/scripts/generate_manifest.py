#!/usr/bin/env python3
"""Builds the ESP Web Tools manifest.json and stages firmware binaries for
GitHub Pages, reading flash offsets from ESP-IDF's own build/flasher_args.json
rather than hardcoding them, so it stays correct regardless of sdkconfig
(partition table, flash size) changes.
"""
import json
import os
import shutil

BUILD_DIR = "build"
SITE_DIR = "site"


def main():
    with open(os.path.join(BUILD_DIR, "flasher_args.json")) as f:
        flasher_args = json.load(f)

    os.makedirs(SITE_DIR, exist_ok=True)

    parts = []
    for offset_str, rel_path in flasher_args["flash_files"].items():
        filename = os.path.basename(rel_path)
        shutil.copy(os.path.join(BUILD_DIR, rel_path), os.path.join(SITE_DIR, filename))
        parts.append({"path": filename, "offset": int(offset_str, 16)})
    parts.sort(key=lambda p: p["offset"])

    version = os.environ.get("GITHUB_SHA", "dev")[:7]

    manifest = {
        "name": "ESP32-ADSB-Radar",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {"chipFamily": "ESP32", "parts": parts},
        ],
    }

    with open(os.path.join(SITE_DIR, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)


if __name__ == "__main__":
    main()
