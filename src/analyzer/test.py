import os
import subprocess

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
    '-e', emulator_root,
    '-p', virtual_sample, analysis_sample,
    virtual_sample
]

result = subprocess.run(command, cwd=os.getcwd())

exit(result.returncode)
