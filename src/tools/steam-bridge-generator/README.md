# Steam bridge code generator

Generates the two halves of the Sogen Steam paravirtualization bridge from a Steamworks
`steam_api.json`:

| Output | Consumed by | Contents |
|---|---|---|
| `steam_shim_interfaces.generated.hpp` | guest `steam-shim` (`steam_api64.dll`) | one proxy class per versioned interface; each virtual method marshals its args over `\\.\SogenSteam` |
| `steam_interfaces.generated.hpp` | host `SogenSteam` device | descriptor table (interface/version → ordered methods → per-param marshalling info) used to translate pointer/buffer args across the guest↔host boundary |

This mirrors `tools/vulkan-bridge-generator`. The cut is at the C++ interface boundary — the same
seam Proton's `lsteamclient` uses — and both sides are derived from the *same* method order, so the
guest proxy's vtable matches the SDK interface the game was compiled against.

## Running

```sh
python generate.py --spec /path/to/steam_api.json --out-dir generated
```

With no `--spec`, it uses `steam_api.mock.json` (a tiny MIT-safe stand-in) so the bridge builds
without the SDK.

## ⚠️ Licensing — read before pointing this at the real SDK

The real `steam_api.json` ships in Valve's **Steamworks SDK** (gated behind a Steamworks account at
`partner.steamgames.com`) and is **not redistributable**. Neither are stubs mechanically derived from
it. Proton may ship SDK-derived wrappers because it is Valve's own shipping vehicle; that cover does
**not** extend to a third-party emulator. This exact space is legally live — Valve DMCA'd Goldberg in
2024.

Therefore:

- **Do not commit** SDK-derived generated output. Run the generator locally against your own SDK copy
  and keep `generated/` (when produced from the real spec) out of version control.
- The committed `steam_api.mock.json` and any output derived *from it* are original to this repo and
  safe to check in as a build/demo fixture.

## Coverage / TODO

The prototype's type model (`SCALAR_SIZES`, `classify_param`, `classify_return`) covers the
scalar / out-buffer / string shapes the mock exercises. Unmapped types degrade to a 4-byte scalar with
a `// TODO(codegen)` marker in the output rather than failing, so a partial SDK still generates and the
gaps are visible. Extend the type model as the covered interface surface grows. Buffer-length pairings
that `steam_api.json` does not express structurally (the `pBuffer`/`cbBuffer` convention) are the one
place a small hand-maintained annotation layer is still needed.
