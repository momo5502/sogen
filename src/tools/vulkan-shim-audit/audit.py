#!/usr/bin/env python3
"""Check that the guest Vulkan shim implements every command the bridge advertises.

The shim reports Vulkan 1.3 to the guest, so every core command up to 1.3 must resolve through its
vkGet*ProcAddr table: a guest that trusts the version calls whatever it reports. Commands are read
from vk.xml and matched by name against the shim's proc table, so a core command and the extension
aliases it was promoted from are checked separately.
"""

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ADVERTISED_CORE_VERSIONS = ("1_0", "1_1", "1_2", "1_3")


def shim_commands(shim_source):
    return set(re.findall(r'\{\.name = "(vk\w+)"', shim_source))


def core_commands(registry):
    commands = set()
    for feature in registry.findall("feature"):
        if "vulkan" not in (feature.get("api") or "").split(","):
            continue
        if not feature.get("name", "").endswith(ADVERTISED_CORE_VERSIONS):
            continue
        for require in feature.findall("require"):
            commands.update(command.get("name") for command in require.findall("command"))
    return commands


def main():
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=root / "deps/Vulkan-Headers/registry/vk.xml")
    parser.add_argument("--shim", type=Path, default=root / "src/samples/vulkan-shim/vulkan_shim.cpp")
    args = parser.parse_args()

    registry = ET.parse(args.registry).getroot()
    implemented = shim_commands(args.shim.read_text(encoding="utf-8"))

    missing = sorted(core_commands(registry) - implemented)
    for command in missing:
        print(f"core command missing from the shim: {command}")

    if missing:
        return 1

    print("Vulkan shim covers every advertised core command")
    return 0


if __name__ == "__main__":
    sys.exit(main())
