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
import json
import os
import re
from dataclasses import dataclass, field


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


def interface_classnames_from_headers(sdk_dir: str) -> list[str]:
    """Recover the ordered list of ISteam* interface class names from an SDK's headers, for old SDKs that
    predate steam_api.json. Only classes that actually declare virtuals become interfaces (parse_interface
    filters the rest)."""
    names: list[str] = []
    for fn in sorted(os.listdir(sdk_dir)):
        if fn.startswith("isteam") and fn.endswith(".h"):
            with open(os.path.join(sdk_dir, fn), "r", encoding="utf-8", errors="replace") as f:
                content = strip_comments(f.read())
            for mm in re.finditer(r"\bclass\s+(ISteam\w+)\s*(?:final\b)?\s*\{", content):
                if mm.group(1) not in names:
                    names.append(mm.group(1))
    return names


def build_interfaces(sdk_dir: str, spec: dict, types: Types,
                     classnames: list[str] | None = None) -> tuple[list[Interface], list[str]]:
    # Concatenate every interface header once; each class is found by name.
    text = ""
    for fn in sorted(os.listdir(sdk_dir)):
        if fn.startswith("isteam") and fn.endswith(".h"):
            with open(os.path.join(sdk_dir, fn), "r", encoding="utf-8", errors="replace") as f:
                text += "\n" + f.read()
    text = strip_comments(text)  # before brace matching: doc comments can contain unbalanced { }

    # The class->version map. From json when available (the latest SDK); for an old SDK (no json) the
    # class list is recovered from the headers directly and the version comes from the header define.
    if classnames is None:
        versions = {i["classname"]: i.get("version_string", "") for i in spec["interfaces"]}
    else:
        versions = {cn: "" for cn in classnames}
    # Direction/shape attributes live in json (headers only give types), keyed by (class, method, param).
    # An old interface reuses the latest json's attributes: they are keyed by (class, method) name, which
    # is stable across versions, so out-parameter directions carry over for same-named methods.
    attr_map: dict[tuple[str, str], dict[str, dict]] = {}
    for i in spec["interfaces"]:
        for m in i["methods"]:
            attr_map[(i["classname"], m["methodname"])] = {
                p["paramname"]: {k: v for k, v in p.items() if k not in ("paramname", "paramtype", "paramtype_flat", "desc")}
                for p in m.get("params", [])
            }

    # Interface version strings from the headers' `#define <X>_INTERFACE_VERSION "SteamX0NN"`. json omits
    # version_string for some interfaces (e.g. ISteamClient), so the header is the reliable source.
    header_version_strings = re.findall(r'_INTERFACE_VERSION\s+"([^"]+)"', text)

    def header_version(classname: str) -> str:
        # Match on the normalized interface family so BOTH naming conventions resolve: the short
        # "SteamUserStats013" and the long "STEAMUSERSTATS_INTERFACE_VERSION006" both key to STEAMUSERSTATS.
        target = family_key(classname[1:])  # drop the leading 'I'
        for v in header_version_strings:
            if family_key(v) == target:
                return v
        return ""
    # Struct/class types with an actual definition ({ ... }) are complete; forward-only declarations are
    # not. Scan ALL SDK headers (not just isteam*.h) so structs from matchmakingtypes.h / steamclientpublic.h
    # / steamnetworkingtypes.h etc. are known and their single-pointer params can be marshalled by value.
    for fn in sorted(os.listdir(sdk_dir)):
        if fn.endswith(".h"):
            with open(os.path.join(sdk_dir, fn), "r", encoding="utf-8", errors="replace") as f:
                content = strip_comments(f.read())
            for mm in re.finditer(r"\b(?:struct|class)\s+(\w+)\s*(?:final\b)?\s*(?::[^{;]*)?\{", content):
                types.aggregates.add(mm.group(1))

    interfaces, skipped = [], []
    for classname, version in versions.items():
        version = version or header_version(classname)  # json omits some; recover from the header define
        hmethods = parse_interface(text, classname)
        if not hmethods:
            skipped.append(classname)
            continue
        methods = []
        for idx, hm in enumerate(hmethods):
            mattrs = attr_map.get((classname, hm.name), {})
            param_names = {n for (_t, n) in hm.params if n}
            ptr_params = {n for (t, n) in hm.params if n and ("*" in t or "&" in t)}
            params = [classify_param(types, t, n, mattrs.get(n, {}), param_names, ptr_params) if n
                      else Param("", t, "complex") for (t, n) in hm.params]
            infer_conventional_buffers(types, params)
            rk = classify_return(types, hm.ret)
            # Non-public methods (protected STEAM_PRIVATE_API) can't be called through an external
            # pointer host-side, so they are stubbed on both sides (the guest override keeps the class
            # concrete). const params would also declare an uninitialized const host local.
            simple = hm.is_public and rk != "complex" and all(p.kind != "complex" for p in params)
            # A method returning an ISteam* interface pointer alongside a version-string param can be
            # forwarded: the host registers the real interface and returns a handle; the guest wraps it in
            # the matching proxy. Requires all other params marshallable.
            iface_version = ""
            ret_s = hm.ret.strip()
            # Returns an interface pointer: an ISteam* pointer, or a GetISteam* method returning void*
            # (e.g. ISteamClient::GetISteamGenericInterface, how steam_api resolves versioned interfaces).
            returns_iface = (ret_s.startswith("ISteam") and ret_s.endswith("*")) or (
                ret_s in ("void *", "void*") and hm.name.startswith("GetISteam"))
            if not simple and hm.is_public and returns_iface and "**" not in ret_s \
                    and all(p.kind != "complex" for p in params):
                v = next((p.name for p in params if p.kind == "cstr"), "")
                if v:
                    rk = "iface_return"
                    simple = True
                    iface_version = v
            methods.append(Method(hm.name, idx, hm.ret, rk, params, hm.raw_params, hm.const, simple, iface_version))
        nested_enums = set(re.findall(r"\benum\s+(?:class\s+)?(\w+)", extract_class_body(text, classname) or ""))
        interfaces.append(Interface(classname, version, methods, nested_enums))
    return interfaces, skipped


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


def family_key(version: str) -> str:
    """Normalize a versioned interface string to its interface family, dropping the trailing version
    number and any `_INTERFACE_VERSION` decoration so the two naming conventions collapse together:
    "SteamClient008" / "SteamClient023" -> "STEAMCLIENT";
    "STEAMUSERSTATS_INTERFACE_VERSION007" / "SteamUserStats013" -> "STEAMUSERSTATS". Must match the C++
    steam_family_key emitted below exactly."""
    s = version.upper().replace("_", "").rstrip("0123456789")
    for suffix in ("INTERFACEVERSION", "VERSION", "INTERFACE"):
        if s.endswith(suffix):
            return s[: -len(suffix)]
    return s


def marshal_compatible(old_m: "Method", lat_m: "Method") -> bool:
    """True when an old method marshals byte-identically to the latest same-named method, so the old proxy
    can forward over the single (latest) wire protocol using the latest method's index. Requires the same
    return kind and the same ordered (kind, type) for every parameter."""
    if old_m.ret_kind != lat_m.ret_kind or old_m.ret.strip() != lat_m.ret.strip():
        return False
    if len(old_m.params) != len(lat_m.params):
        return False
    for op, lp in zip(old_m.params, lat_m.params):
        if op.kind != lp.kind or op.type != lp.type:
            return False
    return True


def prefix_forward_extras(old_m: "Method", lat_m: "Method") -> "list[Param] | None":
    """If old_m's parameters are a strict prefix of lat_m's (the latest version only APPENDED parameters --
    a common Steamworks pattern, e.g. ISteamNetworking::IsP2PPacketAvailable gained an nChannel), and the
    appended parameters are simple inputs we can default (scalars -> 0, pointers -> null), return that list
    of extra Params. The old proxy then forwards over the single (latest) wire, synthesizing defaults for
    the new trailing args. Returns None when not prefix-forwardable."""
    if old_m.ret_kind != lat_m.ret_kind or old_m.ret.strip() != lat_m.ret.strip():
        return None
    if len(old_m.params) >= len(lat_m.params):
        return None
    for op, lp in zip(old_m.params, lat_m.params):
        if op.kind != lp.kind or op.type != lp.type:
            return None
    extras = lat_m.params[len(old_m.params):]
    if any(e.kind not in ("byval", "cstr", "in_ptr", "null_ptr") for e in extras):
        return None  # an appended out/struct/array param can't be safely defaulted
    return extras


def emit_family_helper() -> list[str]:
    """C++ twin of family_key(): normalizes a runtime version string in place. Kept identical to the
    Python so the emitted family constants line up with what a legacy game passes at runtime."""
    return [
        "    inline void steam_family_key(const char* v, char* out, size_t cap)",
        "    {",
        "        size_t o = 0;",
        "        for (size_t i = 0; v[i] && o + 1 < cap; ++i)",
        "        {",
        "            char c = v[i];",
        "            if (c == '_') continue;",
        "            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');",
        "            out[o++] = c;",
        "        }",
        "        out[o] = '\\0';",
        "        while (o > 0 && out[o - 1] >= '0' && out[o - 1] <= '9') out[--o] = '\\0';",
        "        for (const char* suf : {\"INTERFACEVERSION\", \"VERSION\", \"INTERFACE\"})",
        "        {",
        "            const size_t n = std::strlen(suf);",
        "            if (o >= n && std::strcmp(out + o - n, suf) == 0) { o -= n; out[o] = '\\0'; break; }",
        "        }",
        "    }",
    ]


def emit_guest(interfaces: list[Interface], tag: str = "",
               latest_index: dict[str, dict[str, int]] | None = None,
               latest_methods: dict[str, dict[str, "Method"]] | None = None) -> str:
    # tag == "" is the current SDK (its own vtable + wire indices). A non-empty tag is an OLD SDK compiled
    # in isolation under namespace sogen::steam_shim::<tag>: its proxies present the OLD vtable to the game
    # but forward each call over the SINGLE (latest) wire protocol, using the latest method's index for the
    # same-named method. Methods with no marshalling-compatible latest twin self-stub (report_unsupported).
    ns = "sogen::steam_shim" + (f"::{tag}" if tag else "")
    L = ["#pragma once", "",
         "// GENERATED -- guest Steam proxies (derive from real interfaces; marshal across the bridge).", "",
         '#include "steam_shim_runtime.hpp"', "", f"namespace {ns}", "{",
         "    void* create_proxy(const char* version, uint64_t handle);  // defined below; used by iface-returning methods"]
    for i in interfaces:
        fam = family_key(i.version) if i.version else ""
        cls = f"proxy_{i.classname}"
        L.append(f"    struct {cls} final : public {i.classname}")
        L.append("    {")
        L.append(f"        explicit {cls}(uint64_t handle) : bridge_handle_(handle) {{}}")
        for m in i.methods:
            # Pick the wire index: own index for the latest SDK; the latest same-named method's index for an
            # old SDK, either when the marshalling matches exactly or when the latest only appended trailing
            # args (prefix-forwarded, defaulting the extras). None => forward is impossible; the method stubs.
            extras: "list[Param]" = []
            if tag and latest_methods is not None:
                lat = latest_methods.get(fam, {}).get(m.name)
                wire_idx = None
                if lat is not None and marshal_compatible(m, lat):
                    wire_idx = latest_index[fam][m.name]
                elif lat is not None and (ex := prefix_forward_extras(m, lat)) is not None:
                    wire_idx = latest_index[fam][m.name]
                    extras = ex
            else:
                wire_idx = m.index
            emit_simple = m.simple and wire_idx is not None
            L.append(f"        {signature(m)} override")
            L.append("        {")
            guard = struct_guard(m, "sogen::steam_shim")
            if emit_simple and guard:
                L.append(f"            if constexpr ({guard}) {{")
            if emit_simple:
                L.append(f"            sogen::steam_shim::invoker inv(this->bridge_handle_, {wire_idx});")
                for p in m.params:
                    if p.kind == "byval":
                        L.append(f"            inv.put_scalar(&{p.name}, sizeof({p.name}));")
                    elif p.kind == "byval_struct":
                        L.append(f"            inv.put(&{p.name}, sizeof({p.name}));")
                    elif p.kind == "cstr":
                        # For an old iface-getter, ask the host for the LATEST version of the interface (so a
                        # modern object + latest thunk answer the call); the returned handle is still wrapped
                        # in the OLD proxy below, giving the game the vtable it compiled against.
                        if tag and m.ret_kind == "iface_return" and p.name == m.iface_version:
                            L.append(f"            inv.put_cstr(sogen::steam_shim::latest_version_for({p.name}));")
                        else:
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
                for e in extras:
                    # Trailing parameters the latest interface appended; the older interface has no argument
                    # for them, so send a default (0 / null) matching the old behavior.
                    if e.kind == "byval":
                        L.append("            { const uint64_t _appended = 0; inv.put_scalar(&_appended, sizeof(_appended)); }")
                    elif e.kind == "cstr":
                        L.append('            inv.put_cstr("");')
                    elif e.kind == "in_ptr":
                        L.append("            inv.put_in_ptr(nullptr, 0);")
                    # null_ptr: not marshalled.
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
    if not tag:
        L += emit_family_helper()
        # Map any requested interface version to the LATEST version string of its family. An old proxy uses
        # this to fetch the modern object from the host while still presenting the old vtable to the game.
        L.append("    const char* latest_version_for(const char* requested)")
        L.append("    {")
        L.append("        char fam[80]; steam_family_key(requested, fam, sizeof(fam));")
        seen_lv: set[str] = set()
        for i in interfaces:
            if i.version:
                key = family_key(i.version)
                if key in seen_lv:
                    continue
                seen_lv.add(key)
                L.append(f'        if (std::strcmp(fam, "{key}") == 0) return "{i.version}";')
        L.append("        return requested;")
        L.append("    }")
    L.append("    inline void* create_proxy(const char* version, uint64_t handle)")
    L.append("    {")
    for i in interfaces:
        if i.version:
            L.append(f'        if (std::strcmp(version, "{i.version}") == 0) return new proxy_{i.classname}(handle);')
    if not tag:
        # Legacy-version fallback (latest SDK only): a game requests a version this SDK no longer defines and
        # no old-SDK tag handled it -> serve the modern proxy for the same family (append-only vtables only).
        L.append("        char fam[80]; steam_family_key(version, fam, sizeof(fam));")
        seen: set[str] = set()
        for i in interfaces:
            if i.version:
                key = family_key(i.version)
                if key in seen:
                    continue
                seen.add(key)
                L.append(f'        if (std::strcmp(fam, "{key}") == 0) return new proxy_{i.classname}(handle);')
    L.append("        return nullptr;")
    L.append("    }")
    L += ["} // namespace", ""]
    return "\n".join(L)


def emit_host(interfaces: list[Interface]) -> str:
    L = ["#pragma once", "",
         "// GENERATED -- host dispatch (calls the real interface method; compiler emits correct ABI).", "",
         "namespace sogen::steam_host", "{"]
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
    L += emit_family_helper()
    L.append("    inline int dispatch(const char* version, void* iface_ptr, uint32_t method, "
             "steam_host_reader& in, steam_host_writer& out)")
    L.append("    {")
    for i in interfaces:
        if i.version:
            L.append(f'        if (std::strcmp(version, "{i.version}") == 0) '
                     f"return dispatch_{i.classname}(iface_ptr, method, in, out);")
    L.append("        // Legacy-version fallback (mirrors the guest create_proxy): route an older interface")
    L.append("        // version to the same-family modern dispatcher.")
    L.append("        char fam[80]; steam_family_key(version, fam, sizeof(fam));")
    seen: set[str] = set()
    for i in interfaces:
        if i.version:
            key = family_key(i.version)
            if key in seen:
                continue
            seen.add(key)
            L.append(f'        if (std::strcmp(fam, "{key}") == 0) '
                     f"return dispatch_{i.classname}(iface_ptr, method, in, out);")
    L.append("        return steam_host_unknown_interface;")
    L.append("    }")
    L.append("")
    # Resolve an (interface version, method index) to the method name, for tracing. Uses the same family
    # fallback as dispatch(), so a legacy version string maps onto the latest interface's method list.
    L.append("    inline const char* method_name(const char* version, uint32_t method)")
    L.append("    {")
    for i in interfaces:
        if i.version and i.methods:
            names = ", ".join(f'"{m.name}"' for m in i.methods)
            L.append(f"        static const char* const _names_{i.classname}[] = {{{names}}};")
    L.append("        char fam[80]; steam_family_key(version, fam, sizeof(fam));")
    seen_n: set[str] = set()
    for i in interfaces:
        if i.version and i.methods:
            key = family_key(i.version)
            if key in seen_n:
                continue
            seen_n.add(key)
            L.append(f'        if (std::strcmp(fam, "{key}") == 0) '
                     f'return method < {len(i.methods)}u ? _names_{i.classname}[method] : "?";')
    L.append('        return "?";')
    L.append("    }")
    L += ["} // namespace", ""]
    return "\n".join(L)


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, help="path to the latest sdk/public/steam (has steam_api.json)")
    ap.add_argument("--out-dir", default=os.path.join(here, "generated"))
    ap.add_argument("--old-sdk", action="append", default=[], metavar="TAG=PATH",
                    help="an older SDK header dir compiled in isolation as namespace <TAG> (repeatable), e.g. "
                         "v105=/path/to/lsteamclient/steamworks_sdk_105. Old proxies forward to the latest wire.")
    args = ap.parse_args()

    with open(os.path.join(args.sdk, "steam_api.json"), "r", encoding="utf-8") as f:
        spec = json.load(f)
    types = Types(spec)
    interfaces, skipped = build_interfaces(args.sdk, spec, types)
    os.makedirs(args.out_dir, exist_ok=True)

    # Latest method index + Method object per (interface family, method name), so an old proxy can forward a
    # same-named call over the single (latest) wire protocol.
    latest_index: dict[str, dict[str, int]] = {}
    latest_methods: dict[str, dict[str, "Method"]] = {}
    for i in interfaces:
        fam = family_key(i.version) if i.version else ""
        latest_index.setdefault(fam, {})
        latest_methods.setdefault(fam, {})
        for m in i.methods:
            latest_index[fam][m.name] = m.index
            latest_methods[fam][m.name] = m
            # The latest SDK renames methods it keeps only for backwards compatibility with a "_DEPRECATED"
            # suffix (e.g. ISteamUser::InitiateGameConnection, which IWNet uses to auth with a server). The
            # method and its ABI are unchanged, so alias the un-suffixed name back to it for old-version
            # forwarding; a real un-suffixed method (should one exist) keeps priority via setdefault.
            if m.name.endswith("_DEPRECATED"):
                base = m.name[: -len("_DEPRECATED")]
                latest_index[fam].setdefault(base, m.index)
                latest_methods[fam].setdefault(base, m)

    # .hxx extension keeps the generated files out of clang-format.
    outputs = {
        "steam_versions.generated.hxx": emit_versions(interfaces),
        "steam_shim_proxies.generated.hxx": emit_guest(interfaces),
        "steam_host_thunks.generated.hxx": emit_host(interfaces),
    }
    for name, text in outputs.items():
        with open(os.path.join(args.out_dir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    total = sum(len(i.methods) for i in interfaces)
    simple = sum(1 for i in interfaces for m in i.methods if m.simple)
    print(f"interfaces {len(interfaces)} (skipped {len(skipped)}: {skipped})")
    print(f"methods {total}; marshalled {simple} ({100 * simple // max(total,1)}%), stubbed {total - simple}")

    for entry in args.old_sdk:
        tag, _, path = entry.partition("=")
        tag, path = tag.strip(), path.strip()
        if not tag or not path:
            raise SystemExit(f"--old-sdk expects TAG=PATH, got {entry!r}")
        old_names = interface_classnames_from_headers(path)
        old_ifaces, old_skipped = build_interfaces(path, spec, types, classnames=old_names)
        forwarded = sum(1 for i in old_ifaces for m in i.methods if m.simple
                        and (lm := latest_methods.get(family_key(i.version) if i.version else "", {}).get(m.name))
                        and marshal_compatible(m, lm))
        old_total = sum(len(i.methods) for i in old_ifaces)
        tag_dir = os.path.join(args.out_dir, tag)
        os.makedirs(tag_dir, exist_ok=True)
        with open(os.path.join(tag_dir, "steam_shim_proxies.generated.hxx"), "w", encoding="utf-8", newline="\n") as f:
            f.write(emit_guest(old_ifaces, tag=tag, latest_index=latest_index, latest_methods=latest_methods))
        print(f"[{tag}] interfaces {len(old_ifaces)} (skipped {old_skipped}); "
              f"methods {old_total}; forwarded {forwarded}, stubbed {old_total - forwarded}")


if __name__ == "__main__":
    main()
