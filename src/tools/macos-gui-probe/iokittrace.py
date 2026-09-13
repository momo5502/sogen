# lldb breakpoint callbacks that dump every IOKit MIG message iokitprobe sends and the reply that came
# back. IOKit's stubs call mach_msg2_internal directly rather than mach_msg, so a DYLD_INSERT_LIBRARIES
# interposer records none of them and this is the only way to see the wire.
#
#   WIRE_OUT=./iokit-wire.txt lldb -b \
#     -o "command script import ./iokittrace.py" -o "target create ./iokitprobe" \
#     -o "breakpoint set -n phase -s iokitprobe" -o "breakpoint command add -F iokittrace.on_phase 1" \
#     -o "breakpoint set -n mach_msg2_internal -s libsystem_kernel.dylib" \
#     -o "breakpoint command add -F iokittrace.on_msg 2" -o run -o quit
#
# fail_server_version, attached to IOKit's io_server_version, forces the refusal that makes the client
# fall back from the _bin routine family to the XML one.
import lldb, struct, os

PATH = os.environ.get('WIRE_OUT', './iokit-wire.txt')
OUT = open(PATH, 'w')
phase = ['?']
pending = {}

def hexdump(proc, addr, n, tag):
    if n <= 0 or n > 0x10000:
        OUT.write("  %s: <size %d>\n" % (tag, n)); return
    err = lldb.SBError()
    data = proc.ReadMemory(addr, n, err)
    if data is None:
        OUT.write("  %s: <unreadable %s>\n" % (tag, err)); return
    OUT.write("  %s (%d bytes)\n" % (tag, n))
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        OUT.write("    %04x  %s  |%s|\n" % (i,
            ' '.join('%02x' % b for b in chunk).ljust(47),
            ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)))

def on_phase(frame, bp_loc, d):
    p = frame.FindRegister('x0').GetValueAsUnsigned()
    s = frame.GetThread().GetProcess().ReadCStringFromMemory(p, 256, lldb.SBError())
    phase[0] = s
    OUT.write("\n===== PHASE %s =====\n" % s)
    OUT.flush()
    return False

def fail_server_version(frame, bp_loc, d):
    thread = frame.GetThread()
    err = lldb.SBError()
    v = frame.EvaluateExpression("(int)1")
    thread.ReturnFromFrame(frame, v)
    return False

def on_return(frame, bp_loc, d):
    proc = frame.GetThread().GetProcess()
    info = pending.pop(frame.GetPC(), None)
    if info is None:
        return False
    buf, rcv_size, inband = info
    ret = frame.FindRegister('x0').GetValueAsUnsigned() & 0xffffffff
    hdr = proc.ReadMemory(buf, 24, lldb.SBError())
    rsize = struct.unpack('<I', hdr[4:8])[0] if hdr else 0
    OUT.write("  return 0x%x  reply_size %d\n" % (ret, rsize))
    hexdump(proc, buf, min(rsize + 8, rcv_size), "REPLY(+trailer)")
    for (addr, n, tag) in inband:
        hexdump(proc, addr, n, tag)
    OUT.flush()
    return False

def on_msg(frame, bp_loc, d):
    thread = frame.GetThread()
    proc = thread.GetProcess()
    buf = frame.FindRegister('x0').GetValueAsUnsigned()
    x2 = frame.FindRegister('x2').GetValueAsUnsigned()
    x4 = frame.FindRegister('x4').GetValueAsUnsigned()
    x6 = frame.FindRegister('x6').GetValueAsUnsigned()
    send_size = x2 >> 32
    msgid = x4 >> 32
    rcv_size = x6 & 0xffffffff
    if not (2790 <= msgid <= 2900):
        return False
    OUT.write("--- msgid %d (0x%x) send_size %d rcv_size %d [phase %s]\n" % (msgid, msgid, send_size, rcv_size, phase[0]))
    hexdump(proc, buf, send_size, "REQUEST")
    # remember the client's inband buffer pointer for the _buf routines so the reply dump shows what
    # the kernel wrote into it
    inband = []
    data = proc.ReadMemory(buf, send_size, lldb.SBError())
    if msgid in (2888, 2889) and data and send_size >= 16:
        addr = struct.unpack('<Q', data[send_size-16:send_size-8])[0]
        inband.append((addr, 2048, "CLIENT BUF @0x%x" % addr))
    parent = thread.GetFrameAtIndex(1)
    if parent.IsValid():
        ra = parent.GetPC()
        pending[ra] = (buf, rcv_size, inband)
        bp = proc.GetTarget().BreakpointCreateByAddress(ra)
        bp.SetScriptCallbackFunction('iokittrace.on_return')
        bp.SetOneShot(True)
    OUT.flush()
    return False
