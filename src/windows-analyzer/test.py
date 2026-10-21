import os
import struct
import subprocess
import tempfile

emulator_root = os.getenv('EMULATOR_ROOT')
analysis_sample = os.getenv('ANALYSIS_SAMPLE')
virtual_sample = 'C:/analysis-sample.exe'

application = 'analyzer'

def make_app(app):
    if os.name == 'nt':
        return app + ".exe"

    return app

command = [
    os.path.join(os.getcwd(), make_app(application)),
    '-c',
    '--network', 'live',
    '-e', emulator_root,
    '--root-mode', 'rw',
    '-p', virtual_sample, analysis_sample,
    virtual_sample
]

with tempfile.TemporaryDirectory(prefix='sogen-old-snapshot-') as directory:
    snapshot = os.path.join(directory, 'old-format.snap')
    with open(snapshot, 'wb') as stream:
        stream.write(struct.pack('<4sII', b'SNAP', 3, 0x8664))
    rejected = subprocess.run(
        [command[0], '--snapshot', snapshot, '-e', emulator_root],
        capture_output=True, text=True
    )
    assert rejected.returncode != 0
    assert 'Unsupported snapshot version: 3' in rejected.stdout + rejected.stderr

result = subprocess.run(command, cwd=os.getcwd())

exit(result.returncode)
