import os
import subprocess
import sys

fixture = os.environ['MACOS_TRACE_FIXTURE']
application = 'analyzer.exe' if os.name == 'nt' else 'analyzer'

command = [
    os.path.join(os.getcwd(), application),
    '--os=macos',
    '--max-instructions', '200000',
    fixture,
]

result = subprocess.run(command, cwd=os.getcwd(), capture_output=True, text=True)
output = result.stdout + result.stderr

expected = [
    'Executing syscall: open (0x5) at 0x',
    '--> path: "/sogen-trace-fixture/no-such-file"',
    '--> flags: O_RDONLY',
    '--> Failed: ENOENT (2)',
    'Executing syscall: write (0x4) at 0x',
    '--> fd: 1',
    '--> cbuf: "Hello, sogen!\\n"',
    '--> nbyte: 14',
    'Hello, sogen!',
    'Executing syscall: exit (0x1) at 0x',
    '--> rval: 0',
]

missing = [line for line in expected if line not in output]

if missing or result.returncode != 0:
    print(output)
    print('exit status: %d' % result.returncode)
    for line in missing:
        print('MISSING: ' + line)
    sys.exit(1)


def run(extra):
    completed = subprocess.run(command[:1] + extra + command[1:], cwd=os.getcwd(), capture_output=True, text=True)
    return completed.stdout + completed.stderr, completed.returncode


# --skip-args isolates the decoder: the syscall lines and the guest's own output stay, the argument rows
# go. The errno line is not an argument row and stays too.
skipped, status = run(['--skip-args'])
if status != 0 or '--> path:' in skipped or 'Executing syscall: open' not in skipped or 'Hello, sogen!' not in skipped:
    print(skipped)
    print('--skip-args did not suppress exactly the argument rows')
    sys.exit(1)

# --ignore drops a call and the rows belonging to it, not one or the other.
ignored, status = run(['-i', 'open'])
if status != 0 or 'Executing syscall: open' in ignored or '--> path:' in ignored or 'Hello, sogen!' not in ignored:
    print(ignored)
    print('--ignore did not suppress the call together with its rows')
    sys.exit(1)

sys.exit(0)
