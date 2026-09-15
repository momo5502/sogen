"""
Tiny native Win32 debugger (ctypes) that captures ALPC reply messages.

It launches a target exe, sets a software breakpoint on
ntdll!NtAlpcSendWaitReceivePort, and for every call dumps the *receive* message
buffer (the raw RPC reply = LRPC header + NDR payload) and the receive message
attributes (where transferred handles ride) once the syscall returns.

Use it to capture the real SYSTEM_AUDIO_STREAM wire reply (opnum 7,
CreateRemoteStream) from a live Windows audio service, so the emulator's
audio_service handler can replay it. Replies are printed with their size and
hex; any reply larger than --min-dump bytes is also saved to a .bin file.

    python alpc_capture.py path\\to\\audio-sample.exe

Requires a session with a working default audio endpoint for the audio capture
to reach CreateRemoteStream.
"""

import ctypes as C
import ctypes.wintypes as W
import os
import sys
import struct
import argparse

k32 = C.WinDLL("kernel32", use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 0x00000002
INFINITE = 0xFFFFFFFF
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001

EXCEPTION_DEBUG_EVENT = 1
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_PROCESS_DEBUG_EVENT = 5

EXCEPTION_BREAKPOINT = 0x80000003
EXCEPTION_SINGLE_STEP = 0x80000004

CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2
CONTEXT_FULL = CONTEXT_CONTROL | CONTEXT_INTEGER | (CONTEXT_AMD64 | 0x8)

TRAP_FLAG = 0x100


class EXCEPTION_RECORD(C.Structure):
    _fields_ = [
        ("ExceptionCode", C.c_uint32),
        ("ExceptionFlags", C.c_uint32),
        ("ExceptionRecord", C.c_uint64),
        ("ExceptionAddress", C.c_uint64),
        ("NumberParameters", C.c_uint32),
        ("_pad", C.c_uint32),
        ("ExceptionInformation", C.c_uint64 * 15),
    ]


class EXCEPTION_DEBUG_INFO(C.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", C.c_uint32)]


class CREATE_PROCESS_DEBUG_INFO(C.Structure):
    _fields_ = [
        ("hFile", C.c_void_p),
        ("hProcess", C.c_void_p),
        ("hThread", C.c_void_p),
        ("lpBaseOfImage", C.c_void_p),
        ("dwDebugInfoFileOffset", C.c_uint32),
        ("nDebugInfoSize", C.c_uint32),
        ("lpThreadLocalBase", C.c_void_p),
        ("lpStartAddress", C.c_void_p),
        ("lpImageName", C.c_void_p),
        ("fUnicode", C.c_uint16),
    ]


class DEBUG_EVENT_U(C.Union):
    _fields_ = [
        ("Exception", EXCEPTION_DEBUG_INFO),
        ("CreateProcessInfo", CREATE_PROCESS_DEBUG_INFO),
        ("_buf", C.c_byte * 160),
    ]


class DEBUG_EVENT(C.Structure):
    _fields_ = [
        ("dwDebugEventCode", C.c_uint32),
        ("dwProcessId", C.c_uint32),
        ("dwThreadId", C.c_uint32),
        ("u", DEBUG_EVENT_U),
    ]


class M128A(C.Structure):
    _fields_ = [("Low", C.c_uint64), ("High", C.c_int64)]


class CONTEXT(C.Structure):
    _pack_ = 16
    _fields_ = [
        ("P1Home", C.c_uint64), ("P2Home", C.c_uint64), ("P3Home", C.c_uint64),
        ("P4Home", C.c_uint64), ("P5Home", C.c_uint64), ("P6Home", C.c_uint64),
        ("ContextFlags", C.c_uint32), ("MxCsr", C.c_uint32),
        ("SegCs", C.c_uint16), ("SegDs", C.c_uint16), ("SegEs", C.c_uint16),
        ("SegFs", C.c_uint16), ("SegGs", C.c_uint16), ("SegSs", C.c_uint16),
        ("EFlags", C.c_uint32),
        ("Dr0", C.c_uint64), ("Dr1", C.c_uint64), ("Dr2", C.c_uint64),
        ("Dr3", C.c_uint64), ("Dr6", C.c_uint64), ("Dr7", C.c_uint64),
        ("Rax", C.c_uint64), ("Rcx", C.c_uint64), ("Rdx", C.c_uint64),
        ("Rbx", C.c_uint64), ("Rsp", C.c_uint64), ("Rbp", C.c_uint64),
        ("Rsi", C.c_uint64), ("Rdi", C.c_uint64), ("R8", C.c_uint64),
        ("R9", C.c_uint64), ("R10", C.c_uint64), ("R11", C.c_uint64),
        ("R12", C.c_uint64), ("R13", C.c_uint64), ("R14", C.c_uint64),
        ("R15", C.c_uint64), ("Rip", C.c_uint64),
        ("FltSave", C.c_byte * 512),
        ("VectorRegister", M128A * 26),
        ("VectorControl", C.c_uint64),
        ("DebugControl", C.c_uint64), ("LastBranchToRip", C.c_uint64),
        ("LastBranchFromRip", C.c_uint64), ("LastExceptionToRip", C.c_uint64),
        ("LastExceptionFromRip", C.c_uint64),
    ]


class STARTUPINFOW(C.Structure):
    _fields_ = [
        ("cb", C.c_uint32), ("lpReserved", C.c_wchar_p), ("lpDesktop", C.c_wchar_p),
        ("lpTitle", C.c_wchar_p), ("dwX", C.c_uint32), ("dwY", C.c_uint32),
        ("dwXSize", C.c_uint32), ("dwYSize", C.c_uint32), ("dwXCountChars", C.c_uint32),
        ("dwYCountChars", C.c_uint32), ("dwFillAttribute", C.c_uint32), ("dwFlags", C.c_uint32),
        ("wShowWindow", C.c_uint16), ("cbReserved2", C.c_uint16), ("lpReserved2", C.c_void_p),
        ("hStdInput", C.c_void_p), ("hStdOutput", C.c_void_p), ("hStdError", C.c_void_p),
    ]


class PROCESS_INFORMATION(C.Structure):
    _fields_ = [("hProcess", C.c_void_p), ("hThread", C.c_void_p),
                ("dwProcessId", C.c_uint32), ("dwThreadId", C.c_uint32)]


def read_mem(hproc, addr, size):
    buf = (C.c_char * size)()
    n = C.c_size_t(0)
    if not k32.ReadProcessMemory(hproc, C.c_void_p(addr), buf, size, C.byref(n)):
        return None
    return bytes(buf[: n.value])


def write_mem(hproc, addr, data):
    n = C.c_size_t(0)
    ok = k32.WriteProcessMemory(hproc, C.c_void_p(addr), data, len(data), C.byref(n))
    k32.FlushInstructionCache(hproc, C.c_void_p(addr), len(data))
    return bool(ok)


def get_context(hthread):
    ctx = CONTEXT()
    ctx.ContextFlags = CONTEXT_FULL
    if not k32.GetThreadContext(hthread, C.byref(ctx)):
        raise OSError("GetThreadContext failed %d" % C.get_last_error())
    return ctx


def set_context(hthread, ctx):
    if not k32.SetThreadContext(hthread, C.byref(ctx)):
        raise OSError("SetThreadContext failed %d" % C.get_last_error())


def hexdump(data, limit=4096):
    out = []
    for i in range(0, min(len(data), limit), 16):
        chunk = data[i:i + 16]
        out.append("%04x: %s" % (i, " ".join("%02x" % b for b in chunk)))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target")
    ap.add_argument("--min-dump", type=int, default=200,
                    help="save replies whose payload exceeds this many bytes")
    ap.add_argument("--outdir", default=None,
                    help="where to save captures (default: next to the target exe)")
    args = ap.parse_args()
    target = os.path.abspath(args.target)
    outdir = args.outdir or os.path.dirname(target)
    print("[*] target: %s" % target)
    print("[*] captures will be saved to: %s" % outdir)

    k32.GetThreadContext.argtypes = [C.c_void_p, C.c_void_p]
    k32.SetThreadContext.argtypes = [C.c_void_p, C.c_void_p]
    k32.OpenThread.restype = C.c_void_p
    k32.GetModuleHandleW.restype = C.c_void_p
    k32.GetProcAddress.restype = C.c_void_p
    k32.GetProcAddress.argtypes = [C.c_void_p, C.c_char_p]
    k32.ReadProcessMemory.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
    k32.WriteProcessMemory.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]

    ntdll = k32.GetModuleHandleW("ntdll.dll")
    bp_addr = k32.GetProcAddress(C.c_void_p(ntdll), b"NtAlpcSendWaitReceivePort")
    map_addr = k32.GetProcAddress(C.c_void_p(ntdll), b"NtMapViewOfSection")
    dup_addr = k32.GetProcAddress(C.c_void_p(ntdll), b"NtDuplicateObject")
    print("[*] AlpcSendWaitReceive=0x%x MapView=0x%x Duplicate=0x%x" % (bp_addr, map_addr, dup_addr))

    si = STARTUPINFOW()
    si.cb = C.sizeof(si)
    pi = PROCESS_INFORMATION()
    if not k32.CreateProcessW(target, None, None, None, False,
                              DEBUG_ONLY_THIS_PROCESS, None, None, C.byref(si), C.byref(pi)):
        raise OSError("CreateProcessW failed %d" % C.get_last_error())

    hproc = None
    orig = {}           # addr -> original byte
    rearm = {}          # tid -> addr to re-arm after single-step
    pending = {}        # ret_addr -> (kind, args)
    entry_bps = {}      # function entry addr -> kind ("alpc"/"map"/"dup")
    ret_bp_set = set()
    captured = [0]
    audio_handles = set()   # handle values referring to the shared render section
    section_dumped = [0]

    def ru64(addr):
        b = read_mem(hproc, addr, 8)
        return int.from_bytes(b, "little") if b and len(b) == 8 else 0

    def set_bp(addr):
        if addr in orig:
            return
        b = read_mem(hproc, addr, 1)
        orig[addr] = b
        write_mem(hproc, addr, b"\xcc")

    def thread(tid):
        return k32.OpenThread(0x1FFFFF, False, tid)

    evt = DEBUG_EVENT()
    while True:
        if not k32.WaitForDebugEvent(C.byref(evt), INFINITE):
            break
        code = evt.dwDebugEventCode
        status = DBG_CONTINUE

        if code == CREATE_PROCESS_DEBUG_EVENT:
            hproc = evt.u.CreateProcessInfo.hProcess
            entry_bps[bp_addr] = "alpc"  # map/dup armed lazily once a section handle is delivered
            set_bp(bp_addr)
            print("[*] process created, ALPC breakpoint armed")

        elif code == EXCEPTION_DEBUG_EVENT:
            er = evt.u.Exception.ExceptionRecord
            ec = er.ExceptionCode
            addr = er.ExceptionAddress
            tid = evt.dwThreadId

            if ec == EXCEPTION_BREAKPOINT and addr in orig:
                ht = thread(tid)
                ctx = get_context(ht)
                ctx.Rip = addr
                # restore original byte so the instruction can execute
                write_mem(hproc, addr, orig[addr])
                rsp = ctx.Rsp

                if addr in entry_bps:
                    kind = entry_bps[addr]
                    ret = ru64(rsp)
                    if kind == "alpc":
                        send_msg = ctx.R8
                        recv_msg = ru64(rsp + 0x28)
                        recv_attr = ru64(rsp + 0x38)
                        opnum = -1
                        send_head = b""
                        if send_msg:
                            sd = read_mem(hproc, send_msg + 0x28, 0x30) or b""
                            send_head = sd[:24]
                            if len(sd) >= 22:
                                opnum = int.from_bytes(sd[20:22], "little")
                        if recv_msg:
                            pending[ret] = ("alpc", (recv_msg, recv_attr, opnum, send_head))
                    elif kind == "map":
                        # NtMapViewOfSection(SectionHandle=rcx, Process=rdx, BaseAddress=r8, ZeroBits=r9,
                        #   CommitSize=[rsp+0x28], SectionOffset=[rsp+0x30], ViewSize=[rsp+0x38], ...)
                        pending[ret] = ("map", (ctx.Rcx, ctx.R8, ru64(rsp + 0x38)))
                    elif kind == "dup":
                        # NtDuplicateObject(SrcProc=rcx, SrcHandle=rdx, TgtProc=r8, TgtHandle=r9, ...)
                        pending[ret] = ("dup", (ctx.Rdx, ctx.R9))
                    if ret not in ret_bp_set:
                        set_bp(ret)
                        ret_bp_set.add(ret)

                elif addr in pending:
                    kind, pargs = pending.pop(addr)
                    if kind == "alpc":
                        recv_msg, recv_attr, opnum, send_head = pargs
                        hdr = read_mem(hproc, recv_msg, 0x28)
                        data_len = int.from_bytes(hdr[0:2], "little")
                        total_len = int.from_bytes(hdr[2:4], "little")
                        payload = read_mem(hproc, recv_msg + 0x28, data_len) or b""
                        attrs = read_mem(hproc, recv_attr, 0x60) if recv_attr else b""
                        # If the reply carried a HANDLE attribute, remember the handle: it's the shared
                        # render section we want to dump once audioses maps it.
                        if attrs and len(attrs) >= 8:
                            alloc, valid = struct.unpack_from("<II", attrs, 0)
                            if valid & 0x10000000:  # ALPC_MESSAGE_HANDLE_ATTRIBUTE
                                off = 8
                                if alloc & 0x80000000: off += 0x20
                                if alloc & 0x40000000: off += 0x20
                                if alloc & 0x20000000: off += 0x18
                                h = int.from_bytes(attrs[off + 8:off + 16], "little")
                                audio_handles.add(h)
                                print("[handle] op%d delivered section handle 0x%x" % (opnum, h))
                                if map_addr not in entry_bps:
                                    entry_bps[map_addr] = "map"
                                    entry_bps[dup_addr] = "dup"
                                    set_bp(map_addr)
                                    set_bp(dup_addr)
                                    print("[*] section handle seen -> map/dup breakpoints armed")
                        print("[reply] opnum=%d data_len=%d send=%s" % (opnum, data_len, send_head.hex()))
                        if data_len >= args.min_dump:
                            fn = os.path.join(outdir, "alpc_reply_%02d_op%d_len%d.bin" % (captured[0], opnum, data_len))
                            with open(fn, "wb") as f:
                                f.write(payload)
                            if attrs:
                                with open(fn[:-4] + "_attrs.bin", "wb") as f:
                                    f.write(attrs)
                            print("[+] saved %s" % fn)
                            captured[0] += 1
                    elif kind == "dup":
                        src, tgt_ptr = pargs
                        if src in audio_handles and tgt_ptr:
                            tgt = ru64(tgt_ptr)
                            if tgt:
                                audio_handles.add(tgt)
                                print("[handle] duplicated section handle 0x%x -> 0x%x" % (src, tgt))
                    elif kind == "map":
                        sect, base_ptr, size_ptr = pargs
                        base = ru64(base_ptr) if base_ptr else 0
                        size = ru64(size_ptr) if size_ptr else 0
                        tagged = sect in audio_handles
                        print("[map] handle=0x%x base=0x%x size=0x%x%s" % (sect, base, size, " <AUDIO>" if tagged else ""))
                        # Dump the tracked audio section, and any sizable view mapped after op7 (the shared
                        # audio buffer is hundreds of KB) so we can identify it offline by content.
                        if base and (tagged or size >= 0x20000):
                            view = read_mem(hproc, base, min(size or 0x1000, 0x80000)) or b""
                            tag = "audio" if tagged else "h%x" % sect
                            fn = os.path.join(outdir, "render_section_%02d_%s_size%d.bin" % (section_dumped[0], tag, size))
                            with open(fn, "wb") as f:
                                f.write(view)
                            print("[SECTION] handle 0x%x @0x%x size=0x%x -> %s" % (sect, base, size, fn))
                            print(hexdump(view, 0x80))
                            section_dumped[0] += 1

                # single-step over the restored instruction, then re-arm
                ctx.EFlags |= TRAP_FLAG
                set_context(ht, ctx)
                rearm[tid] = addr
                k32.CloseHandle(ht)

            elif ec == EXCEPTION_SINGLE_STEP and tid in rearm:
                a = rearm.pop(tid)
                write_mem(hproc, a, b"\xcc")
            else:
                status = DBG_EXCEPTION_NOT_HANDLED

        elif code == EXIT_PROCESS_DEBUG_EVENT:
            k32.ContinueDebugEvent(evt.dwProcessId, evt.dwThreadId, DBG_CONTINUE)
            break

        k32.ContinueDebugEvent(evt.dwProcessId, evt.dwThreadId, status)

    print("[*] done, %d large reply(ies) captured" % captured[0])
    return captured[0]


if __name__ == "__main__":
    main()
    sys.stdout.flush()
    os._exit(0)
