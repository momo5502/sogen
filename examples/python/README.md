# Python examples

`examples/python/basic_usage.py` shows how to run the bundled Windows `test-sample.exe`.
`examples/python/linux_true.py` shows how to run `/bin/true` with the Linux bindings.

Install the package:

```bash
pip install sogen
```

Download an emulation root:

https://sogen.dev/root.zip

Then run the Windows example:

```bash
python examples/python/basic_usage.py
```

Or run the Linux example:

```bash
python examples/python/linux_true.py
```

Set `EMULATOR_ROOT` to the extracted root. If unset, the script uses `./root`.
Set `LINUX_EMULATION_ROOT` to a Linux sysroot if `/` is not suitable; the default example uses `/` and `/bin/true` on Linux hosts.
