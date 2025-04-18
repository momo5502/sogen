import os
import sys
import subprocess

emulator_root = os.getenv('EMULATOR_ROOT')
analysis_sample = os.getenv('ANALYSIS_SAMPLE')
virtual_sample = 'C:/analysis-sample.exe'

application = 'analyzer'

is_node = len(sys.argv) > 1 and sys.argv == "node"

def make_app(app):
    if is_node:
        return app + ".js"

    if os.name == 'nt':
        return app + ".exe"

    return app

command = [
    os.path.join(os.getcwd(), make_app(application)),
    '-c',
    '-e', emulator_root,
    '-p', virtual_sample, analysis_sample,
    virtual_sample
]

if is_node:
    command = ["node"] + command

result = subprocess.run(command, cwd=os.getcwd())

exit(result.returncode)
