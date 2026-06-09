#!/usr/bin/env python3
"""Generate (de)serialization for Vulkan structs from vk.xml for the Sogen GPU bridge.

The bridge forwards Vulkan calls from the guest to the host's real driver. Pointer-rich
parameter structs (pNext chains, string arrays, nested structs) cannot ride the wire as raw
bytes, so this generator emits matching `encode`/`decode` for an allowlist of structs into a
linear byte stream -- the same blueprint Mesa's Venus uses.

`encode` (guest side) flattens a struct into the stream; `decode` (host side) rebuilds it,
allocating pointees from an arena that outlives the host call. The two are symmetric and share
one generated header, so they can never drift.

This runs offline; the generated header is checked in. Regenerate with the `vulkan-bridge-generate`
CMake target (or by running this script) whenever the allowlist or vk.xml changes.
"""

import argparse
import sys
import xml.etree.ElementTree as ET

# Structs to generate marshalling for. Extend as the remoted surface grows. Order does not matter;
# dependencies are emitted as long as they are also listed here.
STRUCT_ALLOWLIST = [
    "VkApplicationInfo",
    "VkInstanceCreateInfo",
]


class Member:
    def __init__(self, name, type_name, ptr_level, length, optional, has_values):
        self.name = name
        self.type_name = type_name
        self.ptr_level = ptr_level
        self.length = length          # raw len= attribute (or None)
        self.optional = optional
        self.has_values = has_values  # True for sType (values= attribute)

    @property
    def is_pnext(self):
        return self.name == "pNext"

    @property
    def is_string(self):
        return self.type_name == "char" and self.ptr_level == 1 and self.length == "null-terminated"

    @property
    def is_string_array(self):
        return self.type_name == "char" and self.ptr_level == 2 and self.length and self.length.endswith("null-terminated")

    @property
    def is_struct_ptr(self):
        # Single optional pointer to a struct, no array length.
        return self.ptr_level == 1 and not self.length and self.type_name != "char" and self.type_name != "void"

    def length_member(self):
        # For "enabledLayerCount,null-terminated" -> "enabledLayerCount".
        return self.length.split(",")[0] if self.length else None


def parse_member(member_el):
    type_el = member_el.find("type")
    name_el = member_el.find("name")
    type_name = type_el.text if type_el is not None else None
    name = name_el.text if name_el is not None else None

    # Pointer level = number of '*' across the member's text fragments.
    ptr_level = "".join(member_el.itertext()).count("*")

    return Member(
        name=name,
        type_name=type_name,
        ptr_level=ptr_level,
        length=member_el.get("len"),
        optional=member_el.get("optional") == "true",
        has_values=member_el.get("values") is not None,
    )


def parse_structs(xml_path):
    root = ET.parse(xml_path).getroot()
    structs = {}
    for type_el in root.iter("type"):
        if type_el.get("category") != "struct":
            continue
        name = type_el.get("name")
        members = [parse_member(m) for m in type_el.findall("member")]
        structs[name] = members
    return structs


def emit_encode(name, members):
    lines = [f"    inline void encode(writer& w, const {name}& value)", "    {"]
    for m in members:
        if m.is_pnext:
            lines.append("        encode_pnext(w, value.pNext);")
        elif m.is_string:
            lines.append(f"        encode_string(w, value.{m.name});")
        elif m.is_string_array:
            lines.append(f"        encode_string_array(w, value.{m.name}, value.{m.length_member()});")
        elif m.is_struct_ptr:
            lines.append(f"        if (value.{m.name})")
            lines.append("        {")
            lines.append("            w.flag(true);")
            lines.append(f"            encode(w, *value.{m.name});")
            lines.append("        }")
            lines.append("        else")
            lines.append("        {")
            lines.append("            w.flag(false);")
            lines.append("        }")
        else:
            lines.append(f"        w.scalar(value.{m.name});")
    lines.append("    }")
    return "\n".join(lines)


def emit_decode(name, members):
    lines = [f"    inline bool decode(reader& r, arena& a, {name}& value)", "    {", "        value = {};"]
    for m in members:
        if m.is_pnext:
            lines.append("        value.pNext = nullptr;")
            lines.append("        if (!decode_pnext(r)) { return false; }")
        elif m.is_string:
            lines.append(f"        value.{m.name} = decode_string(r, a);")
        elif m.is_string_array:
            lines.append(
                f"        value.{m.name} = decode_string_array(r, a, value.{m.length_member()});")
        elif m.is_struct_ptr:
            lines.append("        {")
            lines.append("            bool present = false;")
            lines.append("            if (!r.flag(present)) { return false; }")
            lines.append("            if (present)")
            lines.append("            {")
            lines.append(f"                auto* pointee = a.allocate_array<{m.type_name}>(1);")
            lines.append("                if (!decode(r, a, *pointee)) { return false; }")
            lines.append(f"                value.{m.name} = pointee;")
            lines.append("            }")
            lines.append("        }")
        else:
            lines.append(f"        if (!r.scalar(value.{m.name})) {{ return false; }}")
    lines.append("        return true;")
    lines.append("    }")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vk-xml", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    structs = parse_structs(args.vk_xml)

    missing = [name for name in STRUCT_ALLOWLIST if name not in structs]
    if missing:
        sys.exit(f"structs not found in vk.xml: {', '.join(missing)}")

    # Forward declarations let encode/decode reference each other regardless of allowlist order.
    forward = []
    for name in STRUCT_ALLOWLIST:
        forward.append(f"    inline void encode(writer& w, const {name}& value);")
        forward.append(f"    inline bool decode(reader& r, arena& a, {name}& value);")

    bodies = []
    for name in STRUCT_ALLOWLIST:
        bodies.append(emit_encode(name, structs[name]))
        bodies.append(emit_decode(name, structs[name]))

    out = []
    out.append("// GENERATED by tools/vulkan-bridge-generator/generate.py -- do not edit by hand.")
    out.append("// Regenerate with the `vulkan-bridge-generate` CMake target after changing the allowlist or vk.xml.")
    out.append("#pragma once")
    out.append("")
    out.append('#include "vk_bridge_serial.hpp"')
    out.append("")
    out.append("namespace sogen::gpu_bridge::marshal")
    out.append("{")
    out.append("\n".join(forward))
    out.append("")
    out.append("\n\n".join(bodies))
    out.append("}")
    out.append("")

    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
