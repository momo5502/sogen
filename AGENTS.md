# Repository Guidelines

## Project Overview

Sogen is a C++20 Windows user-space emulator.
It produces an `analyzer` binary that analyzes and emulates windows binaries.
Details and commandline options can be found in `src/analyzer/main.cpp`

## Build

For fast iterations during development, build the release preset:

`cmake --build --preset=release`

When fully done implementing a feature, make sure to build the tidy configuration, which includes clang-tidy.
It's very slow, so only use it at the end:

`cmake --build --preset=tidy`

## Smoke tests

Execute `analyzer.exe -s test-sample.exe` for smoke tests using the cmd in the directory of the built preset.
E.g.: `cmd /c "cd build\release\artifacts\ && analyzer.exe -s test-sample.exe"`

Other applications can also be executed in the emulator:
`cmd /c "cd build\release\artifacts\ && analyzer.exe -s path/to/binary.exe arg1 arg2 ..."`

## Code comments

The default is **no comment**. Write code that explains itself through naming and structure instead.

A comment is only justified when it carries information that is *not recoverable from the code*, and that a
competent developer reading this file would otherwise get wrong. In practice that means:

- A non-obvious external constraint: a Windows/NT behaviour, an undocumented structure layout, a hardware or
  emulator quirk that forces the code into a shape that otherwise looks wrong.
- A deliberate deviation: why the obvious approach was *not* taken, when the code alone would make a reader
  want to "fix" it.
- A reference that saves someone a research session: a spec section, an MSDN structure, a known-bug link.

Never write:

- Restatements of the code (`// increment the counter`, `// call the handler`, `// loop over the modules`).
- Section headers or banners inside a function (`// --- setup ---`, `// Step 3: cleanup`).
- Narration of your own edit or reasoning (`// changed to fix X`, `// this is safe because ...`, `// now uses Y`).
  This is talking to the reviewer, not to the next reader, and it is noise the moment the change is merged.
- Docstring-style headers that just spell out the signature in prose.
- Obvious type or scope information (`// pointer to the process`).

If you are unsure whether a comment qualifies, it does not. Leave it out. A change that adds zero comments is a
perfectly good change; a change that sprinkles comments over otherwise readable code will be rejected.

## Development notes

- Run clang-format on changed files to ensure consistent formatting
- Do not use shortcuts or workarounds. Always prefer clean and idiomatic solutions.
