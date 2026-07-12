# Steam bridge code generator

`generate.py` parses each Steamworks SDK header snapshot with libclang and emits both halves of the
bridge for that version: guest proxies (`steam_shim_proxies.generated.hxx`) and host dispatch thunks
(`steam_host_thunks.generated.hxx`). Both come from the same vtable order, so a game gets the exact
interface versions it was built against.

## Running

```sh
python generate.py --tag <name>=<sdk_dir> [--tag ...] --out-dir <dir> --bits <32|64>
```

Needs the `libclang` pip package. CMake runs it at configure time (see the root `CMakeLists.txt`); invoke
it by hand only when iterating on the generator.

## Licensing

The Steamworks SDK headers are Valve-licensed and not redistributable, so neither the headers nor the
generated output are committed -- they are fetched and generated locally.

See `docs/steam-bridge.md` for the full design.
