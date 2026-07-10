#!/usr/bin/env python3
"""Sogen Steam bridge code generator.

Emits both halves of the Steam paravirtualization bridge. The generated code INCLUDES THE REAL SDK
HEADERS and derives the guest proxies from the real ISteam* interfaces / calls the real methods on the
host, so the compiler handles every ABI detail (CSteamID register-in / hidden-ptr-return, floats in XMM,
by-value structs, x86 thiscall). Same approach as Proton's lsteamclient.

The vtable is sourced from the HEADERS, not steam_api.json: json lists non-virtual inline convenience
methods (e.g. ISteamNetworkingUtils::InitRelayNetworkAccess) that are not vtable slots, and omits
STEAM_PRIVATE_API-wrapped virtuals that ARE. We parse `virtual ... = 0;` declarations in declaration
(vtable) order from each interface header; json is used only for the class->version map (and later, for
out-parameter direction attributes and callback structs).

Phase 1 fully marshals "simple" methods (params: by-value scalar / CSteamID / const char*; returns:
void / scalar / CSteamID / const char*). Other methods get compiling stub overrides (so the class stays
concrete) and are reported.

LICENSING: SDK headers / steam_api.json and generated code derived from them are Valve-licensed and NOT
redistributable. Generate locally; keep output out of the tree.

Usage: python generate.py --sdk <sdk/public/steam> --out-dir generated
"""

from __future__ import annotations

import argparse
import os
import re
from dataclasses import dataclass, field

import clang.cindex as cx  # pip install libclang (generate-time only; bundles the native lib)


# --- header parsing: recover the true virtual method list (the vtable) ------------------------------

# Annotation macros that wrap or precede declarations/params; each is `MACRO( ... )` and is removed
# entirely (its balanced-paren argument list included), leaving the following param/decl intact.
ANNOTATION_MACROS = [
    "STEAM_CALL_RESULT", "STEAM_CALL_BACK", "STEAM_OUT_STRING_COUNT", "STEAM_OUT_ARRAY_COUNT",
    "STEAM_ARRAY_COUNT_D", "STEAM_ARRAY_COUNT", "STEAM_OUT_ARRAY_CALL", "STEAM_OUT_BUFFER_COUNT",
    "STEAM_BUFFER_COUNT", "STEAM_DESC", "STEAM_OUT_STRUCT", "STEAM_OUT_STRING", "STEAM_FLAT_NAME",
    "STEAM_IGNOREATTR", "STEAM_OUT_ARRAY_COUNT_ARG",
]


def strip_comments(s: str) -> str:
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.DOTALL)
    s = re.sub(r"//[^\n]*", " ", s)
    return s


def _find_matching(s: str, open_idx: int) -> int:
    """Given index of a '(', return index of its matching ')'."""
    depth = 0
    for i in range(open_idx, len(s)):
        if s[i] == "(":
            depth += 1
        elif s[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def remove_macro(s: str, macro: str, unwrap: bool) -> str:
    """Remove `macro( ... )`. If unwrap, keep the inner content (drop only the macro token + parens)."""
    out = []
    i = 0
    while True:
        m = re.search(r"\b" + re.escape(macro) + r"\s*\(", s[i:])
        if not m:
            out.append(s[i:])
            break
        start = i + m.start()
        paren = i + m.end() - 1
        close = _find_matching(s, paren)
        if close == -1:
            out.append(s[i:])
            break
        out.append(s[i:start])
        if unwrap:
            out.append(s[paren + 1:close])
        i = close + 1
    return "".join(out)


def mark_private(s: str) -> str:
    """Emulate STEAM_PRIVATE_API(...)'s real expansion: it flips access to protected around its body
    (`protected: ... public:`). Keeping that lets access tracking treat those methods as non-public."""
    out, i = [], 0
    while True:
        m = re.search(r"\bSTEAM_PRIVATE_API\s*\(", s[i:])
        if not m:
            out.append(s[i:])
            break
        start = i + m.start()
        paren = i + m.end() - 1
        close = _find_matching(s, paren)
        if close == -1:
            out.append(s[i:])
            break
        out.append(s[i:start])
        out.append(" protected: ")
        out.append(s[paren + 1:close])
        out.append(" public: ")
        i = close + 1
    return "".join(out)


def preprocess(body: str) -> str:
    body = strip_comments(body)
    body = mark_private(body)
    for mac in ANNOTATION_MACROS:
        body = remove_macro(body, mac, unwrap=False)
    return body


def split_top(param_text: str) -> list[str]:
    """Split a parameter list on top-level commas (ignoring commas inside ()/<> ). """
    parts, depth, cur = [], 0, ""
    for ch in param_text:
        if ch in "(<":
            depth += 1
        elif ch in ")>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


def strip_defaults(param_text: str) -> str:
    """Remove default-argument clauses; keep exact type/name text otherwise (verbatim header spelling)."""
    parts = split_top(param_text)
    cleaned = [re.sub(r"=\s*.+$", "", p, flags=re.DOTALL).strip() for p in parts]
    return ", ".join(cleaned)


def split_params(param_text: str) -> list[tuple[str, str]]:
    """Parse into (type, name); name is '' for unnamed params (e.g. function-pointer / deprecated)."""
    param_text = param_text.strip()
    if param_text in ("", "void"):
        return []
    out = []
    for p in split_top(param_text):
        p = re.sub(r"=\s*.+$", "", p, flags=re.DOTALL).strip()
        # A function-pointer or otherwise irregular param has no clean trailing identifier name.
        if "(" in p:
            out.append((p, ""))
            continue
        m = re.match(r"^(.*?)([A-Za-z_]\w*)$", p, flags=re.DOTALL)
        if not m or not m.group(1).strip():
            out.append((p, ""))
        else:
            out.append((m.group(1).strip(), m.group(2)))
    return out


@dataclass
class HMethod:
    ret: str
    name: str
    params: list[tuple[str, str]]  # (type, name)
    raw_params: str                # exact header param text (defaults stripped) for the override signature
    const: bool
    is_public: bool                # non-public (protected STEAM_PRIVATE_API) methods can't be called host-side


def extract_class_body(text: str, classname: str) -> str | None:
    # Find the definition (a '{' before the next ';'), skipping forward declarations (`class X;`).
    brace = -1
    for m in re.finditer(r"\bclass\s+" + re.escape(classname) + r"\b", text):
        semi = text.find(";", m.end())
        cand = text.find("{", m.end())
        if cand != -1 and (semi == -1 or cand < semi):
            brace = cand
            break
    if brace == -1:
        return None
    depth, i = 0, brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:i]
        i += 1
    return None


def parse_interface(text: str, classname: str) -> list[HMethod] | None:
    body = extract_class_body(text, classname)
    if body is None:
        return None
    body = preprocess(body)
    labels = [(mm.start(), mm.group(1)) for mm in re.finditer(r"\b(public|protected|private)\s*:", body)]

    def access_at(pos: int) -> str:
        acc = "private"  # class default before any access label
        for lp, la in labels:
            if lp < pos:
                acc = la
            else:
                break
        return acc

    methods = []
    # Each pure virtual: from 'virtual' up to '= 0;'.
    # [^{};] keeps each match within one declaration: it can't cross a `{}` (inline body, e.g. a
    # non-pure virtual destructor) or a `;` (a non-pure virtual) into the next method's `= 0`.
    for m in re.finditer(r"\bvirtual\b([^{};]*?)=\s*0\s*;", body):
        method_access = access_at(m.start())
        decl = m.group(1).strip()
        # decl = "<ret> <name>( <params> ) [const]". Find the top-level parameter parens.
        p_open = decl.find("(")
        if p_open == -1:
            continue
        p_close = _find_matching(decl, p_open)
        if p_close == -1:
            continue
        head = decl[:p_open].strip()
        params_text = decl[p_open + 1:p_close]
        tail = decl[p_close + 1:].strip()
        if "~" in head:
            continue  # virtual destructor: the derived proxy's implicit dtor overrides it; not a bridge method
        nm = re.search(r"([A-Za-z_]\w*)$", head)
        if not nm:
            continue
        name = nm.group(1)
        ret = head[:nm.start()].strip()
        methods.append(HMethod(ret, name, split_params(params_text), strip_defaults(params_text),
                               const=("const" in tail), is_public=(method_access == "public")))
    return methods


# --- type classification ----------------------------------------------------------------------------

BASE_SCALARS = {
    "int", "unsigned int", "uint32", "int32", "uint16", "int16", "unsigned short", "short",
    "uint8", "int8", "char", "unsigned char", "bool", "float", "double", "size_t",
    "uint64", "int64", "long long", "unsigned long long", "long", "unsigned long",
    "CSteamID", "CGameID", "SteamAPICall_t",
}
FLOATING = {"float", "double"}


class Types:
    def __init__(self, spec: dict):
        self.typedefs = {t["typedef"]: t["type"] for t in spec.get("typedefs", [])}
        self.enums = {e["enumname"] for e in spec.get("enums", []) if "enumname" in e}
        # Aggregate (struct/class) types known to be COMPLETE, so sizeof / value-copy is valid. Seeded
        # from json; header-defined struct/class names are added in build_interfaces. Types that are only
        # forward-declared (e.g. SteamDatagramHostedAddress) stay out and their pointer params are stubbed.
        self.aggregates = {s["struct"] for s in spec.get("structs", []) if "struct" in s}
        self.aggregates |= {s["struct"] for s in spec.get("callback_structs", []) if "struct" in s}

    # Types whose definition exists in the headers only behind a disabled #ifdef (niche optional
    # features), so a text scan sees a definition the compiler does not. Treated as incomplete.
    FORWARD_ONLY = {
        "ScePadTriggerEffectParam", "SteamDatagramHostedAddress", "SteamNetworkingFakeIPResult_t",
        "SteamDatagramRelayAuthTicket", "SteamDatagramGameCoordinatorServerLogin",
    }

    def is_complete(self, t: str) -> bool:
        t = t.strip()
        if t in self.FORWARD_ONLY or self.resolve(t) in self.FORWARD_ONLY:
            return False
        if self.is_byvalue_scalar(t) or self.is_enum(t):
            return True
        return t in self.aggregates or self.resolve(t) in self.aggregates

    def resolve(self, t: str) -> str:
        t = t.strip()
        seen = 0
        while t in self.typedefs and seen < 16:
            t = self.typedefs[t].strip()
            seen += 1
        return t

    def is_enum(self, t: str) -> bool:
        t = t.strip()
        if t in self.enums or self.resolve(t) in self.enums:
            return True
        # Steam's naming convention: an enum is E<Name> (e.g. EHTMLMouseButton). json occasionally omits
        # nested enums from its list, so honor the convention for a plain (non-pointer) E-type.
        base = t.replace("const", "").strip()
        return bool(re.match(r"^E[A-Z]\w*$", base))

    def is_byvalue_scalar(self, t: str) -> bool:
        t = t.strip()
        if "*" in t or "&" in t:
            return False
        if self.is_enum(t):
            return True
        base = t.replace("const", "").strip()
        if base in BASE_SCALARS or self.resolve(base) in BASE_SCALARS:
            return True
        # Opaque handle typedef (e.g. HServerListRequest = void*): pass the pointer value opaquely as an
        # 8-byte scalar. Round-trips on a 64-bit guest (a 32-bit guest would truncate -- niche).
        return base in self.typedefs and self.resolve(base).replace(" ", "") == "void*"

    def is_floating(self, t: str) -> bool:
        return self.resolve(t.replace("const", "").strip()) in FLOATING


@dataclass
class Param:
    name: str
    type: str   # for pointer/array kinds this is the element type; otherwise the full type
    kind: str   # byval | cstr | in_ref | in_ptr | out_single | in_array | out_array | out_string | complex
    count: str = ""  # array/string count expression (a param name or a C constant), for array/string kinds


@dataclass
class Method:
    name: str
    index: int
    ret: str
    ret_kind: str
    params: list[Param]
    raw_params: str
    const: bool
    simple: bool
    iface_version: str = ""  # for ret_kind == "iface_return": the version-string param to proxy with


@dataclass
class Interface:
    classname: str
    version: str
    methods: list[Method] = field(default_factory=list)
    nested_enums: set = field(default_factory=set)  # enums declared inside the class (need qualification host-side)


# Attribute keys that mark a pointer as an array/buffer (needs a runtime count) -- deferred to a later
# phase; their presence forces the method to a stub for now.
ARRAY_BUFFER_ATTRS = ("array_count", "out_array_count", "buffer_count", "out_buffer_count",
                      "out_string_count", "out_string")


def element_type(t: str) -> str:
    """Strip one level of pointer/reference and const to get the pointed-to value type."""
    e = t.strip()
    e = re.sub(r"[*&]\s*$", "", e).strip()
    e = re.sub(r"\bconst\b", "", e).strip()
    return e


def classify_param(types: Types, ptype: str, pname: str, attrs: dict, param_names: set[str],
                   ptr_params: set[str]) -> Param:
    t = ptype.strip()
    if t in ("const char *", "const char*"):
        return Param(pname, t, "cstr")
    if types.is_byvalue_scalar(t):
        return Param(pname, t, "byval")
    # A complete struct passed BY VALUE (not a pointer/ref): serialize its bytes, exactly like CSteamID but
    # for any struct. The compiler handles the by-value ABI (register or hidden-reference) on both sides.
    if "*" not in t and "&" not in t:
        base = t.replace("const", "").strip()
        if not base.startswith("ISteam") and not types.is_byvalue_scalar(base) and types.is_complete(base):
            return Param(pname, t, "byval_struct")
    # Pointer to a game-implemented callback/response interface (ISteam...Response *): the guest registers
    # its object, the host substitutes a proxy that routes calls back (reverse channel).
    ebase = element_type(t)
    if t.endswith("*") and ebase.startswith("ISteam") and ebase.endswith("Response"):
        return Param(pname, ebase, "callback_token")
    if "**" in t:
        return Param(pname, t, "null_ptr")  # double-pointer array (e.g. server filters): pass nullptr
    elem = element_type(t)

    # A count expression must resolve at the call site: either a real scalar param of THIS method, or a
    # C constant usable verbatim (headers are included). Pointer params (in/out sizes) are deferred, and
    # json sometimes names a param that doesn't exist in the method -- reject those (a convention pass may
    # still recover the buffer from the adjacent size param).
    def count_ok(tok: str) -> bool:
        if not tok or tok in ptr_params:
            return False
        if tok in param_names:
            return True
        return tok.isdigit() or tok.startswith("k_") or tok[:1].isupper()  # looks like a constant

    # Array / string / buffer parameters, driven by json direction attributes.
    if "out_string_count" in attrs and t.endswith("*") and count_ok(attrs["out_string_count"]):
        return Param(pname, "char", "out_string", attrs["out_string_count"])
    if "array_count" in attrs and count_ok(attrs["array_count"]) and elem not in ("", "void") and "*" not in elem \
            and not elem.startswith("ISteam") and types.is_complete(elem):
        return Param(pname, elem, "in_array", attrs["array_count"])
    if "out_array_count" in attrs and count_ok(attrs["out_array_count"]) and elem not in ("", "void") \
            and "*" not in elem and not elem.startswith("ISteam") and types.is_complete(elem):
        return Param(pname, elem, "out_array", attrs["out_array_count"])
    # Byte buffers (void*/uint8*/...) sized by a scalar count param. When the count is instead a pointer
    # (an in/out size), defer -- count_ok already excludes that case.
    if "buffer_count" in attrs and count_ok(attrs["buffer_count"]) and t.endswith("*"):
        return Param(pname, t, "in_buffer", attrs["buffer_count"])
    if "out_buffer_count" in attrs and count_ok(attrs["out_buffer_count"]) and t.endswith("*"):
        return Param(pname, t, "out_buffer", attrs["out_buffer_count"])
    if any(a in attrs for a in ("buffer_count", "out_buffer_count")):
        return Param(pname, t, "complex")  # pointer-sized (in/out) count: later phase

    # Single struct/scalar pointer or reference. Element must be a concrete, COMPLETE, copyable value type.
    # A char/byte pointer is a string or buffer that needs an explicit size (handled above via attrs);
    # without one we must NOT treat it as a single element -- the callee may write far more than one byte
    # (buffer overflow). Same for void*. Such unsized pointers are stubbed.
    char_like = {"char", "unsigned char", "signed char", "void", "wchar_t", "uint8", "int8"}
    if elem in ("", "void") or "*" in elem or elem.startswith("ISteam") or not types.is_complete(elem) \
            or types.resolve(elem) in char_like or elem in char_like:
        return Param(pname, t, "complex")
    if t.endswith("&"):
        return Param(pname, elem, "in_ref" if t.startswith("const") else "complex")
    if t.endswith("*"):
        # const T* is an input struct; a plain T* is treated as a single out-parameter (Steam's
        # dominant convention for non-const single pointers).
        return Param(pname, elem, "in_ptr" if t.startswith("const") else "out_single")
    return Param(pname, t, "complex")


# Game-implemented response interfaces routed via the reverse-callback channel; id shared with the host.
RESPONSE_IFACE_ID = {
    "ISteamMatchmakingServerListResponse": 0,
    "ISteamMatchmakingPingResponse": 1,
    "ISteamMatchmakingPlayersResponse": 2,
    "ISteamMatchmakingRulesResponse": 3,
}

INT_TYPES = {"int", "unsigned int", "uint32", "int32", "uint16", "int16", "unsigned short", "short",
             "uint8", "int8", "uint64", "int64", "size_t", "long", "unsigned long", "char", "unsigned char"}
BYTE_ELEMS = {"void", "char", "unsigned char", "signed char", "uint8", "int8", "wchar_t"}


def is_integer_type(types: Types, t: str) -> bool:
    return types.resolve(t.replace("const", "").strip()) in INT_TYPES


def size_hint(name: str) -> bool:
    n = name.lower()
    if any(k in n for k in ("size", "count", "len", "num", "bytes")):
        return True
    return bool(re.match(r"^(cb|cub|cch|ce|cu)\w", name))


def infer_conventional_buffers(types: Types, params: list[Param]) -> None:
    """Steam rarely annotates buffer sizes; by convention a byte/array pointer is immediately followed by
    its size (an integer param). Reclassify such unsized `complex` pointers as buffers/arrays so they can
    be marshalled. const -> input, non-const -> output. The size param stays a normal by-value argument
    and doubles as the count (guest and host compute the same size from it)."""
    for j, p in enumerate(params):
        if p.kind != "complex" or not p.type.strip().endswith("*") or "**" in p.type:
            continue
        if j + 1 >= len(params):
            continue
        nxt = params[j + 1]
        if nxt.kind != "byval" or not is_integer_type(types, nxt.type) or not size_hint(nxt.name):
            continue
        elem = element_type(p.type)
        base = elem.replace("const", "").strip()
        is_const = p.type.strip().startswith("const")
        if base in BYTE_ELEMS:
            p.kind = "in_buffer" if is_const else "out_buffer"  # p.type stays the full pointer type
            p.count = nxt.name
        elif elem and "*" not in elem and not elem.startswith("ISteam") and types.is_complete(elem):
            p.type = elem
            p.kind = "in_array" if is_const else "out_array"
            p.count = nxt.name


def classify_return(types: Types, rt: str) -> str:
    rt = rt.strip()
    if rt == "void":
        return "void"
    if rt in ("const char *", "const char*"):
        return "cstr"
    if "*" in rt or "&" in rt:
        return "complex"
    base = types.resolve(rt.replace("const", "").strip())
    if base in ("CSteamID", "CGameID"):
        return "csteamid"
    if types.is_floating(rt):
        return "floating"
    if types.is_byvalue_scalar(rt):
        return "scalar"
    # A complete struct returned BY VALUE (of any size): carried in the out-blob; the compiler handles the
    # return ABI (RAX / hidden pointer) on both sides.
    if "*" not in rt and "&" not in rt:
        base_r = rt.replace("const", "").strip()
        if not base_r.startswith("ISteam") and types.is_complete(base_r):
            return "struct_return"
    # A pointer to a complete struct (e.g. gameserveritem_t*): copy the pointed-to struct back; the guest
    # returns a pointer into a thread-local copy.
    if rt.endswith("*") and "**" not in rt:
        base_r = element_type(rt)
        if base_r and "void" not in base_r and not base_r.startswith("ISteam") and types.is_complete(base_r):
            return "struct_ptr_return"
    return "complex"


def family_key(version: str) -> str:
    """Normalize a versioned interface string to its interface family so the two naming conventions
    collapse together: "SteamUser012" and "STEAMUSERSTATS_INTERFACE_VERSION006" -> STEAMUSER / STEAMUSERSTATS."""
    s = version.upper().replace("_", "").rstrip("0123456789")
    for suffix in ("INTERFACEVERSION", "VERSION", "INTERFACE"):
        if s.endswith(suffix):
            return s[: -len(suffix)]
    return s


# --- libclang front-end: parse one SDK snapshot into the Interface/Method/Param model ---------------
# The STEAM_* annotation macros expand to __attribute__((annotate(...))) only under -DAPI_GEN, so the
# direction/size info (out_buffer_count, array_count, ...) that steam_api.json used to provide is read
# straight from the AST -- letting every snapshot fully describe its own interface versions.

def _parse_tu(sdk_dir: str, bits: int):
    target = "i686-pc-windows-msvc" if bits == 32 else "x86_64-pc-windows-msvc"
    args = ["-x", "c++", "-std=c++14", "-D_WIN32", "-DAPI_GEN", "-fms-extensions",
            "-Wno-ignored-attributes", "-Wno-nonportable-include-path", "-target", target, "-I", sdk_dir]
    src = '#include "steam_api.h"\n#include "steam_gameserver.h"\n'
    return cx.Index.create().parse("s.cpp", args=args, unsaved_files=[("s.cpp", src)],
                                   options=cx.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)


def _build_types(tu) -> Types:
    types = Types({})
    enums, aggregates, typedefs = set(), set(), {}

    def walk(c):
        if c.kind == cx.CursorKind.ENUM_DECL and c.spelling:
            enums.add(c.spelling)
        elif c.kind in (cx.CursorKind.STRUCT_DECL, cx.CursorKind.CLASS_DECL) and c.spelling and c.is_definition():
            aggregates.add(c.spelling)
        elif c.kind == cx.CursorKind.TYPEDEF_DECL and c.spelling:
            typedefs[c.spelling] = c.underlying_typedef_type.spelling
        for ch in c.get_children():
            walk(ch)

    walk(tu.cursor)
    types.enums, types.aggregates, types.typedefs = enums, aggregates, typedefs
    return types


def _param_attrs(param) -> dict:
    attrs = {}
    for ch in param.get_children():
        if ch.kind == cx.CursorKind.ANNOTATE_ATTR:
            for piece in ch.spelling.split(";"):
                if ":" in piece:
                    k, v = piece.split(":", 1)
                    attrs[k.strip()] = v.strip()
    return attrs


def build_interfaces(sdk_dir: str, bits: int = 32) -> tuple[list[Interface], list[str], int]:
    tu = _parse_tu(sdk_dir, bits)
    errors = sum(1 for d in tu.diagnostics if d.severity >= cx.Diagnostic.Error)
    types = _build_types(tu)

    ver_strings = re.findall(r'_INTERFACE_VERSION\s+"([^"]+)"', "".join(
        open(os.path.join(sdk_dir, fn), encoding="utf-8", errors="replace").read()
        for fn in sorted(os.listdir(sdk_dir)) if fn.startswith("isteam") and fn.endswith(".h")))

    def version_of(classname: str) -> str:
        target = family_key(classname[1:])  # drop leading 'I'
        return next((v for v in ver_strings if family_key(v) == target), "")

    def build_method(m, idx: int) -> Method:
        pdecls = [p for p in m.get_children() if p.kind == cx.CursorKind.PARM_DECL]
        param_names = {p.spelling for p in pdecls if p.spelling}
        ptr_params = {p.spelling for p in pdecls if p.spelling and ("*" in p.type.spelling or "&" in p.type.spelling)}
        raw, params = [], []
        for p in pdecls:
            pt, pn = p.type.spelling, p.spelling
            raw.append(f"{pt} {pn}".strip())
            params.append(classify_param(types, pt, pn, _param_attrs(p), param_names, ptr_params)
                          if pn else Param("", pt, "complex"))
        infer_conventional_buffers(types, params)
        rk = classify_return(types, m.result_type.spelling)
        # STEAM_PRIVATE_API methods parse as non-public; they can't be called through an external pointer
        # host-side, so they stub on both sides (the guest override keeps the vtable slot).
        is_public = m.access_specifier == cx.AccessSpecifier.PUBLIC
        simple = is_public and rk != "complex" and all(p.kind != "complex" for p in params)
        iface_version = ""
        ret_s = m.result_type.spelling.strip()
        returns_iface = (ret_s.startswith("ISteam") and ret_s.endswith("*")) or (
            ret_s in ("void *", "void*") and m.spelling.startswith("GetISteam"))
        if not simple and is_public and returns_iface and "**" not in ret_s and all(p.kind != "complex" for p in params):
            v = next((p.name for p in params if p.kind == "cstr"), "")
            if v:
                rk, simple, iface_version = "iface_return", True, v
        return Method(m.spelling, idx, m.result_type.spelling, rk, params, ", ".join(raw),
                      m.is_const_method(), simple, iface_version)

    interfaces, skipped, seen = [], [], set()

    def walk(c):
        if (c.kind == cx.CursorKind.CLASS_DECL and c.spelling.startswith("ISteam")
                and c.is_definition() and c.spelling not in seen):
            methods = [build_method(m, i) for i, m in enumerate(
                mm for mm in c.get_children() if mm.kind == cx.CursorKind.CXX_METHOD and mm.is_virtual_method())]
            if methods:
                seen.add(c.spelling)
                nested = {e.spelling for e in c.get_children() if e.kind == cx.CursorKind.ENUM_DECL and e.spelling}
                interfaces.append(Interface(c.spelling, version_of(c.spelling), methods, nested))
            else:
                skipped.append(c.spelling)
        for ch in c.get_children():
            walk(ch)

    walk(tu.cursor)
    return interfaces, skipped, errors


# --- emit -------------------------------------------------------------------------------------------

def emit_versions(interfaces: list[Interface]) -> str:
    L = ["#pragma once", "", "// GENERATED -- interface class <-> version string + method count.", "",
         "#include <cstdint>", "#include <string_view>", "", "namespace sogen::steam_bridge::gen", "{",
         "    struct interface_version { std::string_view classname; std::string_view version; uint32_t method_count; };",
         "    inline constexpr interface_version interfaces[] = {"]
    for i in interfaces:
        L.append(f'        {{"{i.classname}", "{i.version}", {len(i.methods)}}},')
    L += ["    };", "} // namespace", ""]
    return "\n".join(L)


def signature(m: Method) -> str:
    # Verbatim header parameter text guarantees the override matches the base declaration exactly.
    return f"{m.ret} {m.name}({m.raw_params}){' const' if m.const else ''}"


def struct_guard(m: Method, ns: str) -> str:
    """`is_complete_v<...>` conjunction over a method's struct element types, or '' if none. A type our
    text scan thought complete may actually be forward-declared-only under the build's macros; the guard
    makes such a method self-stub at compile time instead of failing the build."""
    elems = []
    for p in m.params:
        if p.kind in ("in_ref", "in_ptr", "out_single", "in_array", "out_array") and p.type not in elems:
            elems.append(p.type)
    return " && ".join(f"{ns}::is_complete_v<{e}>" for e in elems)


def emit_guest(interfaces: list[Interface], tag: str) -> str:
    L = ["#pragma once", "",
         "// GENERATED -- guest Steam proxies (derive from real interfaces; marshal across the bridge).", "",
         '#include "steam_shim_runtime.hpp"', "", f"namespace sogen::steam_shim::{tag}", "{",
         "    void* create_proxy(const char* version, uint64_t handle);  // defined below; used by iface-returning methods"]
    for i in interfaces:
        cls = f"proxy_{i.classname}"
        L.append(f"    struct {cls} final : public {i.classname}")
        L.append("    {")
        L.append(f"        explicit {cls}(uint64_t handle) : bridge_handle_(handle) {{}}")
        for m in i.methods:
            L.append(f"        {signature(m)} override")
            L.append("        {")
            guard = struct_guard(m, "sogen::steam_shim")
            if m.simple and guard:
                L.append(f"            if constexpr ({guard}) {{")
            if m.simple:
                L.append(f"            sogen::steam_shim::invoker inv(this->bridge_handle_, {m.index});")
                for p in m.params:
                    if p.kind == "byval":
                        L.append(f"            inv.put_scalar(&{p.name}, sizeof({p.name}));")
                    elif p.kind == "byval_struct":
                        L.append(f"            inv.put(&{p.name}, sizeof({p.name}));")
                    elif p.kind == "cstr":
                        L.append(f"            inv.put_cstr({p.name});")
                    elif p.kind == "in_ref":
                        L.append(f"            inv.put_in_ref(&{p.name}, sizeof({p.type}));")
                    elif p.kind == "in_ptr":
                        L.append(f"            inv.put_in_ptr({p.name}, sizeof({p.type}));")
                    elif p.kind == "in_array":
                        L.append(f"            inv.put_var({p.name}, {p.name} ? static_cast<size_t>({p.count}) "
                                 f"* sizeof({p.type}) : 0);")
                    elif p.kind == "in_buffer":
                        L.append(f"            inv.put_var({p.name}, {p.name} ? static_cast<size_t>({p.count}) : 0);")
                    elif p.kind == "callback_token":
                        tid = RESPONSE_IFACE_ID.get(p.type, -1)
                        L.append(f"            inv.put_callback_token(sogen::steam_shim::register_response_object("
                                 f"{p.name}, {tid}), {tid});")
                    # null_ptr: not marshalled; the host substitutes nullptr.
                L.append("            inv.call();")
                # Out-parameters come back in the out-blob, in parameter order (before any string return).
                for p in m.params:
                    if p.kind == "out_single":
                        L.append(f"            inv.get_out({p.name}, sizeof({p.type}));")
                    elif p.kind == "out_array":
                        L.append(f"            inv.get_out({p.name}, static_cast<size_t>({p.count}) * sizeof({p.type}));")
                    elif p.kind == "out_string" or p.kind == "out_buffer":
                        L.append(f"            inv.get_out({p.name}, static_cast<size_t>({p.count}));")
                if m.ret_kind == "cstr":
                    L.append("            return inv.ret_cstr();")
                elif m.ret_kind == "csteamid":
                    L.append(f"            {m.ret} _r; inv.ret_bytes(&_r, sizeof(_r)); return _r;")
                elif m.ret_kind == "floating":
                    L.append(f"            return inv.ret_floating<{m.ret}>();")
                elif m.ret_kind == "scalar":
                    # memcpy (not static_cast) so opaque-pointer handles (void*) convert too.
                    L.append(f"            {m.ret} _r; uint64_t _v = inv.ret_value(); std::memcpy(&_r, &_v, sizeof(_r)); return _r;")
                elif m.ret_kind == "iface_return":
                    L.append("            uint64_t _sub = inv.ret_value();")
                    L.append(f"            return _sub ? reinterpret_cast<{m.ret}>(create_proxy({m.iface_version}, _sub)) : nullptr;")
                elif m.ret_kind == "struct_return":
                    L.append(f"            {m.ret} _r{{}}; inv.get_out(&_r, sizeof(_r)); return _r;")
                elif m.ret_kind == "struct_ptr_return":
                    elem = element_type(m.ret)
                    L.append(f"            static thread_local {elem} _r{{}}; unsigned char _has = 0; inv.get_out(&_has, 1);")
                    L.append("            if (!_has) return nullptr;")
                    L.append("            inv.get_out(&_r, sizeof(_r)); return &_r;")
                if guard:  # close the if constexpr; incomplete-type methods self-stub here
                    L.append("            } else {")
                    L.append(f'                sogen::steam_shim::report_unsupported("{i.classname}", "{m.name}");')
                    if m.ret_kind != "void":
                        L.append("                return {};")
                    L.append("            }")
            else:
                L.append(f'            sogen::steam_shim::report_unsupported("{i.classname}", "{m.name}");')
                if m.ret_kind != "void":
                    L.append("            return {};")
            L.append("        }")
        L.append("        uint64_t bridge_handle_;")
        L.append("    };")
        L.append("")
    L.append("    inline void* create_proxy(const char* version, uint64_t handle)")
    L.append("    {")
    for i in interfaces:
        if i.version:
            L.append(f'        if (std::strcmp(version, "{i.version}") == 0) return new proxy_{i.classname}(handle);')
    L.append("        return nullptr;")
    L.append("    }")
    L += ["} // namespace", ""]
    return "\n".join(L)


def emit_host(interfaces: list[Interface], tag: str) -> str:
    L = ["#pragma once", "",
         "// GENERATED -- host dispatch (calls the real interface method; compiler emits correct ABI).", "",
         f"namespace sogen::steam_host::{tag}", "{"]
    for i in interfaces:
        L.append(f"    inline int dispatch_{i.classname}(void* iface_ptr, uint32_t method, "
                 "steam_host_reader& in, steam_host_writer& out)")
        L.append("    {")
        L.append(f"        auto* self = reinterpret_cast<{i.classname}*>(iface_ptr);")
        L.append("        (void)self; (void)in; (void)out;")
        L.append("        switch (method)")
        L.append("        {")
        for m in i.methods:
            L.append(f"        case {m.index}: // {m.name}")
            L.append("        {")
            guard = struct_guard(m, "sogen::steam_host")
            if m.simple and guard:
                L.append(f"            if constexpr ({guard}) {{")
            if m.simple:
                # A count expression that names a param resolves to that param's host local; a constant is
                # used verbatim. Guest is untrusted, so every guest-influenced allocation is capped.
                local_of = {p.name: f"a{k}" for k, p in enumerate(m.params) if p.name}

                def count_expr(tok: str) -> str:
                    return local_of.get(tok, tok)

                args = []
                out_writes = []
                # Pass 1: read every input into a local. Each is self-delimiting (fixed size, NUL string,
                # or length-prefixed var), so counts are known before any array is sized.
                for k, p in enumerate(m.params):
                    v = f"a{k}"
                    if p.kind == "byval":
                        lt = re.sub(r"\bconst\b", "", p.type).strip()
                        if lt in i.nested_enums:  # nested enum: qualify (host thunk is outside the class)
                            lt = f"{i.classname}::{lt}"
                        L.append(f"            {lt} {v}{{}}; in.get_scalar(&{v}, sizeof({v}));")
                    elif p.kind == "cstr":
                        L.append(f"            const char* {v} = in.get_cstr();")
                    elif p.kind == "in_ref":
                        L.append(f"            {p.type} {v}; in.get(&{v}, sizeof({v}));")
                    elif p.kind == "in_ptr":
                        L.append(f"            {p.type} {v}{{}}; unsigned char {v}_has; in.get(&{v}_has, 1); "
                                 f"if ({v}_has) in.get(&{v}, sizeof({v}));")
                    elif p.kind == "byval_struct":
                        lt = re.sub(r"\bconst\b", "", p.type).strip()
                        L.append(f"            {lt} {v}{{}}; in.get(&{v}, sizeof({v}));")
                    elif p.kind in ("in_array", "in_buffer"):
                        L.append(f"            std::vector<unsigned char> {v}_raw = in.get_var();")
                    elif p.kind == "callback_token":
                        L.append(f"            uint64_t {v}_tok = 0; int32_t {v}_type = 0; "
                                 f"in.get(&{v}_tok, 8); in.get(&{v}_type, 4);")
                # Pass 2: build the call arguments (all input locals now exist; sizes capped).
                for k, p in enumerate(m.params):
                    v = f"a{k}"
                    if p.kind in ("byval", "cstr", "in_ref", "byval_struct"):
                        args.append(v)
                    elif p.kind == "callback_token":
                        L.append(f"            auto* {v} = reinterpret_cast<{p.type}*>("
                                 f"create_response_proxy({v}_type, {v}_tok));")
                        args.append(v)
                    elif p.kind == "null_ptr":
                        args.append("nullptr")
                    elif p.kind == "in_ptr":
                        args.append(f"({v}_has ? &{v} : nullptr)")
                    elif p.kind in ("in_array", "in_buffer"):
                        elem_sizeof = "1" if p.kind == "in_buffer" else f"sizeof({p.type})"
                        cast = p.type if p.kind == "in_buffer" else f"{p.type}*"
                        L.append(f"            std::vector<unsigned char> {v}_buf(cap_bytes(static_cast<size_t>("
                                 f"{count_expr(p.count)}) * {elem_sizeof}));")
                        # Manual min (avoid std::min: windows.h's min macro would break it in some TUs).
                        L.append(f"            std::memcpy({v}_buf.data(), {v}_raw.data(), "
                                 f"{v}_buf.size() < {v}_raw.size() ? {v}_buf.size() : {v}_raw.size());")
                        args.append(f"reinterpret_cast<{cast}>({v}_buf.data())")
                    elif p.kind == "out_single":
                        L.append(f"            {p.type} {v}{{}};")
                        args.append(f"&{v}")
                        out_writes.append(f"            out.put_out(&{v}, sizeof({v}));")
                    elif p.kind == "out_array":
                        L.append(f"            std::vector<{p.type}> {v}_vec(cap_count<{p.type}>("
                                 f"static_cast<size_t>({count_expr(p.count)})));")
                        L.append(f"            {p.type}* {v} = {v}_vec.empty() ? nullptr : {v}_vec.data();")
                        args.append(v)
                        out_writes.append(f"            out.put_out({v}_vec.data(), {v}_vec.size() * sizeof({p.type}));")
                    elif p.kind == "out_string":
                        L.append(f"            size_t {v}_cap = cap_bytes(static_cast<size_t>({count_expr(p.count)}));")
                        L.append(f"            std::vector<char> {v}_vec({v}_cap + 1, 0);")
                        args.append(f"{v}_vec.data()")
                        out_writes.append(f"            out.put_out({v}_vec.data(), {v}_cap);")
                    elif p.kind == "out_buffer":
                        L.append(f"            size_t {v}_cap = cap_bytes(static_cast<size_t>({count_expr(p.count)}));")
                        L.append(f"            std::vector<unsigned char> {v}_vec({v}_cap ? {v}_cap : size_t{{1}});")
                        args.append(f"reinterpret_cast<{p.type}>({v}_vec.data())")
                        out_writes.append(f"            out.put_out({v}_vec.data(), {v}_cap);")
                # Call exactly once (it fills the out locals), then serialize outputs in the guest's read order.
                call = f"self->{m.name}({', '.join(args)})"
                if m.ret_kind == "void":
                    L.append(f"            {call};")
                elif m.ret_kind == "cstr":
                    L.append(f"            const char* _s = {call};")
                elif m.ret_kind == "scalar":
                    L.append(f"            auto _rv = {call}; uint64_t _r = 0; std::memcpy(&_r, &_rv, sizeof(_rv));")
                elif m.ret_kind == "iface_return":
                    L.append(f"            {m.ret} _iface = {call};")
                elif m.ret_kind == "struct_return":
                    L.append(f"            {m.ret} _sret = {call};")
                elif m.ret_kind == "struct_ptr_return":
                    L.append(f"            {m.ret} _pret = {call};")
                else:  # csteamid / floating
                    L.append(f"            {m.ret} _r = {call};")
                L.extend(out_writes)
                if m.ret_kind == "cstr":
                    L.append("            out.put_cstr(_s);")
                elif m.ret_kind == "csteamid":
                    L.append("            out.put_ret(&_r, sizeof(_r));")
                elif m.ret_kind == "floating":
                    L.append("            out.put_ret_floating(_r);")
                elif m.ret_kind == "scalar":
                    L.append("            out.put_ret_value(_r);")
                elif m.ret_kind == "iface_return":
                    L.append(f"            out.put_ret_value(register_returned_interface({local_of[m.iface_version]}, _iface));")
                elif m.ret_kind == "struct_return":
                    L.append("            out.put_out(&_sret, sizeof(_sret));")
                elif m.ret_kind == "struct_ptr_return":
                    L.append("            unsigned char _has = _pret ? 1 : 0; out.put_out(&_has, 1);")
                    L.append("            if (_pret) out.put_out(_pret, sizeof(*_pret));")
                L.append("            return steam_host_ok;")
                if guard:  # incomplete-type methods self-stub at compile time
                    L.append("            } else {")
                    L.append(f'                unsupported("{i.classname}", "{m.name}"); return steam_host_unsupported;')
                    L.append("            }")
            else:
                L.append(f'            unsupported("{i.classname}", "{m.name}"); return steam_host_unsupported;')
            L.append("        }")
        L.append("        default: return steam_host_unknown_method;")
        L.append("        }")
        L.append("    }")
        L.append("")
    L.append("    inline int dispatch(const char* version, void* iface_ptr, uint32_t method, "
             "steam_host_reader& in, steam_host_writer& out)")
    L.append("    {")
    for i in interfaces:
        if i.version:
            L.append(f'        if (std::strcmp(version, "{i.version}") == 0) '
                     f"return dispatch_{i.classname}(iface_ptr, method, in, out);")
    L.append("        return steam_host_unknown_interface;")
    L.append("    }")
    L += ["} // namespace", ""]
    return "\n".join(L)


def emit_tags(tags: list[str]) -> str:
    """A FOR-EACH macro over the built version tags, so the (SDK-header-free) glue TUs can try each tag's
    entry point without including any interface headers."""
    calls = " ".join(f"X({t})" for t in tags)
    return "\n".join([
        "#pragma once", "",
        "// GENERATED -- list of built Steam SDK-version tags.", "",
        f"#define SOGEN_STEAM_TAGS(X) {calls}", "",
    ])


def generate_tag(tag: str, sdk_dir: str, out_dir: str, bits: int) -> tuple[int, int, int]:
    interfaces, skipped, errors = build_interfaces(sdk_dir, bits)
    tag_dir = os.path.join(out_dir, tag)
    os.makedirs(tag_dir, exist_ok=True)
    outputs = {
        "steam_versions.generated.hxx": emit_versions(interfaces),
        "steam_shim_proxies.generated.hxx": emit_guest(interfaces, tag),
        "steam_host_thunks.generated.hxx": emit_host(interfaces, tag),
    }
    for name, text in outputs.items():
        with open(os.path.join(tag_dir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    total = sum(len(i.methods) for i in interfaces)
    simple = sum(1 for i in interfaces for m in i.methods if m.simple)
    return total, simple, errors


def main() -> None:
    ap = argparse.ArgumentParser(description="Version-exact Steam bridge code generator (libclang).")
    ap.add_argument("--tag", action="append", default=[], metavar="NAME=PATH", required=True,
                    help="an SDK snapshot to generate version-exact code for, e.g. v105=/path/to/steamworks_sdk_105 "
                         "(repeatable; NAME becomes the C++ namespace/object-lib tag)")
    ap.add_argument("--out-dir", required=True, help="output root; each tag is written to <out-dir>/<NAME>")
    ap.add_argument("--bits", type=int, default=64, choices=(32, 64), help="target width for parsing (default 64)")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    tags = []
    for entry in args.tag:
        name, _, path = entry.partition("=")
        name, path = name.strip(), path.strip()
        if not name or not path:
            raise SystemExit(f"--tag expects NAME=PATH, got {entry!r}")
        total, simple, errors = generate_tag(name, path, args.out_dir, args.bits)
        tags.append(name)
        print(f"[{name}] parse-errors {errors}; methods {total}; marshalled {simple} "
              f"({100 * simple // max(total, 1)}%), stubbed {total - simple}")

    with open(os.path.join(args.out_dir, "steam_tags.generated.hxx"), "w", encoding="utf-8", newline="\n") as f:
        f.write(emit_tags(tags))


if __name__ == "__main__":
    main()
