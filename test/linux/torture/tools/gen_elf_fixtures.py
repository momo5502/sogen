#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import struct
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_BASE = REPO_ROOT / "test" / "linux" / "hello"
DEFAULT_FIXTURE_ROOT = REPO_ROOT / "test" / "linux" / "torture" / "fixtures" / "elf"


PT_LOAD = 1
PT_DYNAMIC = 2

PF_X = 1
PF_W = 2
PF_R = 4

ELFCLASS64 = 2
ELFDATA2LSB = 1
EM_X86_64 = 62


EHDR_FMT = "<16sHHIQQQIHHHHHH"
PHDR_FMT = "<IIQQQQQQ"
EHDR_SIZE = struct.calcsize(EHDR_FMT)
PHDR_SIZE = struct.calcsize(PHDR_FMT)


def parse_ehdr(data: bytes) -> dict[str, Any]:
    if len(data) < EHDR_SIZE:
        raise ValueError("base ELF is too small")

    unpacked = struct.unpack_from(EHDR_FMT, data, 0)
    return {
        "e_ident": unpacked[0],
        "e_type": unpacked[1],
        "e_machine": unpacked[2],
        "e_version": unpacked[3],
        "e_entry": unpacked[4],
        "e_phoff": unpacked[5],
        "e_shoff": unpacked[6],
        "e_flags": unpacked[7],
        "e_ehsize": unpacked[8],
        "e_phentsize": unpacked[9],
        "e_phnum": unpacked[10],
        "e_shentsize": unpacked[11],
        "e_shnum": unpacked[12],
        "e_shstrndx": unpacked[13],
    }


def read_phdr(
    data: bytes | bytearray, ehdr: dict[str, Any], index: int
) -> tuple[int, int, int, int, int, int, int, int]:
    off = ehdr["e_phoff"] + index * ehdr["e_phentsize"]
    return struct.unpack_from(PHDR_FMT, data, off)


def write_phdr(
    data: bytearray,
    ehdr: dict[str, Any],
    index: int,
    fields: tuple[int, int, int, int, int, int, int, int],
) -> None:
    off = ehdr["e_phoff"] + index * ehdr["e_phentsize"]
    struct.pack_into(PHDR_FMT, data, off, *fields)


def write_fixture(path: pathlib.Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def mutate_wrong_class(base: bytes) -> bytes:
    out = bytearray(base)
    out[4] = 1  # ELFCLASS32
    return bytes(out)


def mutate_wrong_data(base: bytes) -> bytes:
    out = bytearray(base)
    out[5] = 2  # ELFDATA2MSB
    return bytes(out)


def mutate_wrong_machine(base: bytes) -> bytes:
    out = bytearray(base)
    # e_machine at byte offset 18
    struct.pack_into("<H", out, 18, 183)  # EM_AARCH64
    return bytes(out)


def mutate_malformed_pt_dynamic(base: bytes, ehdr: dict[str, Any]) -> bytes:
    out = bytearray(base)
    if ehdr["e_phnum"] == 0:
        return bytes(out)

    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = read_phdr(
        out, ehdr, 0
    )
    p_type = PT_DYNAMIC
    p_offset = len(base) + 0x1000
    p_filesz = 0x80
    p_memsz = 0x80
    write_phdr(
        out,
        ehdr,
        0,
        (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align),
    )
    return bytes(out)


def mutate_overlap_conflict(base: bytes, ehdr: dict[str, Any]) -> bytes:
    out = bytearray(base)
    load_indices: list[int] = []

    for i in range(ehdr["e_phnum"]):
        p = read_phdr(out, ehdr, i)
        if p[0] == PT_LOAD:
            load_indices.append(i)

    if len(load_indices) < 2:
        return bytes(out)

    first = read_phdr(out, ehdr, load_indices[0])
    second = read_phdr(out, ehdr, load_indices[1])

    f_type, f_flags, f_offset, f_vaddr, f_paddr, f_filesz, f_memsz, f_align = first
    s_type, s_flags, s_offset, s_vaddr, s_paddr, s_filesz, s_memsz, s_align = second

    # Force overlap within same page, with conflicting permissions.
    s_vaddr = f_vaddr + 0x800
    s_paddr = s_vaddr
    s_flags = PF_R | PF_W
    if f_flags & PF_W:
        f_flags = PF_R | PF_X

    write_phdr(
        out,
        ehdr,
        load_indices[0],
        (f_type, f_flags, f_offset, f_vaddr, f_paddr, f_filesz, f_memsz, f_align),
    )
    write_phdr(
        out,
        ehdr,
        load_indices[1],
        (s_type, s_flags, s_offset, s_vaddr, s_paddr, s_filesz, s_memsz, s_align),
    )

    return bytes(out)


def mutate_sparse_bss(base: bytes, ehdr: dict[str, Any]) -> bytes:
    out = bytearray(base)

    for i in range(ehdr["e_phnum"]):
        p = read_phdr(out, ehdr, i)
        if p[0] != PT_LOAD:
            continue

        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = p
        p_memsz = max(p_memsz, p_filesz + 0x20000000)  # +512MB logical BSS
        write_phdr(
            out,
            ehdr,
            i,
            (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align),
        )
        break

    return bytes(out)


def mutate_alignment_edge(base: bytes, ehdr: dict[str, Any]) -> bytes:
    out = bytearray(base)

    for i in range(ehdr["e_phnum"]):
        p = read_phdr(out, ehdr, i)
        if p[0] != PT_LOAD:
            continue

        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = p
        p_align = 0x200000
        p_vaddr = p_vaddr + 1  # break p_vaddr%align == p_offset%align invariant
        p_paddr = p_vaddr
        write_phdr(
            out,
            ehdr,
            i,
            (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align),
        )
        break

    return bytes(out)


def generate_manifest(fixture_root: pathlib.Path) -> dict[str, Any]:
    entries = [
        {
            "path": "valid/base_hello.elf",
            "expected": "run",
            "rationale": "Known-good control fixture copied from test/linux/hello.",
        },
        {
            "path": "malformed/truncated_ehdr.elf",
            "expected": "reject",
            "rationale": "ELF header is intentionally truncated.",
        },
        {
            "path": "malformed/truncated_phdr_table.elf",
            "expected": "reject",
            "rationale": "Program header table is intentionally truncated.",
        },
        {
            "path": "malformed/malformed_pt_dynamic.elf",
            "expected": "reject",
            "rationale": "PT_DYNAMIC points beyond file bounds.",
        },
        {
            "path": "malformed/wrong_class.elf",
            "expected": "reject",
            "rationale": "EI_CLASS changed from ELFCLASS64 to ELFCLASS32.",
        },
        {
            "path": "malformed/wrong_data_endianness.elf",
            "expected": "reject",
            "rationale": "EI_DATA changed from little-endian to big-endian.",
        },
        {
            "path": "malformed/wrong_machine.elf",
            "expected": "reject",
            "rationale": "e_machine changed from EM_X86_64 to EM_AARCH64.",
        },
        {
            "path": "near-valid/pt_load_overlap_conflict.elf",
            "expected": "run",
            "rationale": "Two PT_LOAD segments overlap same page with conflicting flags; used to validate deterministic merge behavior.",
        },
        {
            "path": "near-valid/sparse_bss_extreme.elf",
            "expected": "run",
            "rationale": "One PT_LOAD segment has p_memsz >> p_filesz to stress BSS zero-fill behavior.",
        },
        {
            "path": "near-valid/alignment_edge_mismatch.elf",
            "expected": "reject",
            "rationale": "PT_LOAD p_align/p_vaddr mismatch violates expected segment alignment invariants.",
        },
    ]

    return {
        "schema_version": 1,
        "generated_by": "test/linux/torture/tools/gen_elf_fixtures.py",
        "entries": entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate deterministic adversarial ELF fixtures"
    )
    parser.add_argument(
        "--base-binary",
        default=str(DEFAULT_BASE),
        help="Source ELF used as mutation base",
    )
    parser.add_argument(
        "--output-root",
        default=str(DEFAULT_FIXTURE_ROOT),
        help="Fixture output root (contains valid/near-valid/malformed)",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow overwriting existing fixture files",
    )
    args = parser.parse_args()

    base_path = pathlib.Path(args.base_binary).resolve()
    out_root = pathlib.Path(args.output_root).resolve()

    if not base_path.exists():
        raise FileNotFoundError(f"Base binary does not exist: {base_path}")

    base = base_path.read_bytes()
    ehdr = parse_ehdr(base)

    if (
        ehdr["e_ident"][4] != ELFCLASS64
        or ehdr["e_ident"][5] != ELFDATA2LSB
        or ehdr["e_machine"] != EM_X86_64
    ):
        raise ValueError("Base binary must be ELF64 little-endian x86-64")

    out_root.mkdir(parents=True, exist_ok=True)
    for d in ("valid", "near-valid", "malformed"):
        (out_root / d).mkdir(parents=True, exist_ok=True)

    def maybe_write(path: pathlib.Path, payload: bytes) -> None:
        if path.exists() and not args.overwrite:
            raise FileExistsError(
                f"Refusing to overwrite existing fixture without --overwrite: {path}"
            )
        write_fixture(path, payload)

    maybe_write(out_root / "valid" / "base_hello.elf", base)

    maybe_write(out_root / "malformed" / "truncated_ehdr.elf", base[:32])

    phdr_end = ehdr["e_phoff"] + ehdr["e_phentsize"] * ehdr["e_phnum"]
    trunc_phdr_size = max(
        ehdr["e_phoff"] + max(0, ehdr["e_phnum"] - 1) * ehdr["e_phentsize"] + 8,
        EHDR_SIZE,
    )
    trunc_phdr_size = min(trunc_phdr_size, max(EHDR_SIZE, phdr_end - 1))
    maybe_write(
        out_root / "malformed" / "truncated_phdr_table.elf", base[:trunc_phdr_size]
    )

    maybe_write(
        out_root / "malformed" / "malformed_pt_dynamic.elf",
        mutate_malformed_pt_dynamic(base, ehdr),
    )
    maybe_write(out_root / "malformed" / "wrong_class.elf", mutate_wrong_class(base))
    maybe_write(
        out_root / "malformed" / "wrong_data_endianness.elf", mutate_wrong_data(base)
    )
    maybe_write(
        out_root / "malformed" / "wrong_machine.elf", mutate_wrong_machine(base)
    )

    maybe_write(
        out_root / "near-valid" / "pt_load_overlap_conflict.elf",
        mutate_overlap_conflict(base, ehdr),
    )
    maybe_write(
        out_root / "near-valid" / "sparse_bss_extreme.elf",
        mutate_sparse_bss(base, ehdr),
    )
    maybe_write(
        out_root / "near-valid" / "alignment_edge_mismatch.elf",
        mutate_alignment_edge(base, ehdr),
    )

    manifest = generate_manifest(out_root)
    manifest_path = out_root.parent / "manifest.json"
    if manifest_path.exists() and not args.overwrite:
        raise FileExistsError(
            f"Refusing to overwrite existing manifest without --overwrite: {manifest_path}"
        )
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"Generated fixtures under {out_root}")
    print(f"Wrote manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
