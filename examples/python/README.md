# Python example

`examples/python/basic_usage.py` shows the Python bindings in a minimal way:

- create an emulator
- select a backend explicitly
- hook module load / entry-point execution
- run the bundled sample when `EMULATOR_ROOT` and `ANALYSIS_SAMPLE` are set

Run it from a built wheel or an installed package:

```bash
python examples/python/basic_usage.py
```

Environment variables used by the sample path:

- `EMULATOR_ROOT`
- `ANALYSIS_SAMPLE`
