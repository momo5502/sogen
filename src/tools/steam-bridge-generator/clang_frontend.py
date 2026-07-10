"""libclang front-end for the Steam bridge generator (prototype; see docs/steam-bridge-versioning.md).

Produces the same Interface/Method/Param model generate.py's emitter consumes, but from the clang AST --
types, STEAM_* direction annotations (enabled by -DAPI_GEN), and vtable order -- instead of regex +
steam_api.json. This is what makes version-exact, per-version generation possible with no stub/forward
layer: each SDK snapshot fully describes its own interface versions.

Requires `pip install libclang` (generate-time only; bundles the native lib, cross-platform). Run directly
to validate against a snapshot: `python clang_frontend.py <sdk-dir> [32|64]`.
"""
import os
import re
import sys

import generate as g  # sibling module: Types, classify_param/return, infer_conventional_buffers, dataclasses

import clang.cindex as cx

CK = cx.CursorKind


def parse_tu(sdk_dir: str, bits: int = 32):
    index = cx.Index.create()
    target = "i686-pc-windows-msvc" if bits == 32 else "x86_64-pc-windows-msvc"
    args = ["-x", "c++", "-std=c++14", "-D_WIN32", "-DAPI_GEN", "-fms-extensions",
            "-Wno-ignored-attributes", "-Wno-nonportable-include-path", "-target", target, "-I", sdk_dir]
    src = '#include "steam_api.h"\n#include "steam_gameserver.h"\n'
    return index.parse("s.cpp", args=args, unsaved_files=[("s.cpp", src)],
                       options=cx.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)


def build_types(tu) -> "g.Types":
    """A Types instance whose enum / aggregate / typedef sets come from the AST (complete for the snapshot),
    so classify_param's is_enum / is_complete / resolve are accurate rather than heuristic."""
    types = g.Types({})
    enums, aggregates, typedefs = set(), set(), {}

    def walk(c):
        if c.kind == CK.ENUM_DECL and c.spelling:
            enums.add(c.spelling)
        elif c.kind in (CK.STRUCT_DECL, CK.CLASS_DECL) and c.spelling and c.is_definition():
            aggregates.add(c.spelling)
        elif c.kind == CK.TYPEDEF_DECL and c.spelling:
            typedefs[c.spelling] = c.underlying_typedef_type.spelling
        for ch in c.get_children():
            walk(ch)

    walk(tu.cursor)
    types.enums, types.aggregates, types.typedefs = enums, aggregates, typedefs
    return types


def parse_annotations(param) -> dict:
    """STEAM_* annotations on a parameter, as {key: value} (e.g. {'out_buffer_count': 'cubDest'})."""
    attrs = {}
    for ch in param.get_children():
        if ch.kind == CK.ANNOTATE_ATTR:
            for piece in ch.spelling.split(";"):
                if ":" in piece:
                    k, v = piece.split(":", 1)
                    attrs[k.strip()] = v.strip()
    return attrs


def header_version_resolver(sdk_dir: str):
    text = ""
    for fn in sorted(os.listdir(sdk_dir)):
        if fn.startswith("isteam") and fn.endswith(".h"):
            with open(os.path.join(sdk_dir, fn), encoding="utf-8", errors="replace") as f:
                text += f.read()
    strings = re.findall(r'_INTERFACE_VERSION\s+"([^"]+)"', text)

    def version_of(classname: str) -> str:
        target = g.family_key(classname[1:])  # drop leading 'I'
        return next((v for v in strings if g.family_key(v) == target), "")

    return version_of


def build_interfaces(sdk_dir: str, bits: int = 32):
    tu = parse_tu(sdk_dir, bits)
    errors = [d for d in tu.diagnostics if d.severity >= cx.Diagnostic.Error]
    types = build_types(tu)
    version_of = header_version_resolver(sdk_dir)

    interfaces, skipped, seen = [], [], set()

    def build_method(m, idx: int) -> "g.Method":
        param_names = {p.spelling for p in m.get_children() if p.kind == CK.PARM_DECL and p.spelling}
        ptr_params = {p.spelling for p in m.get_children()
                      if p.kind == CK.PARM_DECL and p.spelling and ("*" in p.type.spelling or "&" in p.type.spelling)}
        raw, cparams = [], []
        for p in m.get_children():
            if p.kind != CK.PARM_DECL:
                continue
            pt, pn = p.type.spelling, p.spelling
            raw.append(f"{pt} {pn}".strip())
            if pn:
                cparams.append(g.classify_param(types, pt, pn, parse_annotations(p), param_names, ptr_params))
            else:
                cparams.append(g.Param("", pt, "complex"))
        g.infer_conventional_buffers(types, cparams)
        rk = g.classify_return(types, m.result_type.spelling)
        is_public = m.access_specifier == cx.AccessSpecifier.PUBLIC
        simple = is_public and rk != "complex" and all(p.kind != "complex" for p in cparams)
        iface_version = ""
        ret_s = m.result_type.spelling.strip()
        returns_iface = (ret_s.startswith("ISteam") and ret_s.endswith("*")) or (
            ret_s in ("void *", "void*") and m.spelling.startswith("GetISteam"))
        if not simple and is_public and returns_iface and "**" not in ret_s and all(p.kind != "complex" for p in cparams):
            v = next((p.name for p in cparams if p.kind == "cstr"), "")
            if v:
                rk, simple, iface_version = "iface_return", True, v
        return g.Method(m.spelling, idx, m.result_type.spelling, rk, cparams,
                        ", ".join(raw), m.is_const_method(), simple, iface_version)

    def walk(c):
        if c.kind == CK.CLASS_DECL and c.spelling.startswith("ISteam") and c.is_definition() and c.spelling not in seen:
            methods = []
            for m in c.get_children():
                if m.kind == CK.CXX_METHOD and m.is_virtual_method():
                    methods.append(build_method(m, len(methods)))
            if methods:
                seen.add(c.spelling)
                interfaces.append(g.Interface(c.spelling, version_of(c.spelling), methods))
            else:
                skipped.append(c.spelling)
        for ch in c.get_children():
            walk(ch)

    walk(tu.cursor)
    return interfaces, skipped, len(errors)


if __name__ == "__main__":
    sdk = sys.argv[1]
    ifaces, skipped, nerr = build_interfaces(sdk, int(sys.argv[2]) if len(sys.argv) > 2 else 32)
    total = sum(len(i.methods) for i in ifaces)
    simple = sum(1 for i in ifaces for m in i.methods if m.simple)
    print(f"parse errors: {nerr}")
    print(f"interfaces {len(ifaces)} (skipped {len(skipped)})")
    print(f"methods {total}; marshalled {simple} ({100 * simple // max(total, 1)}%), stubbed {total - simple}")
