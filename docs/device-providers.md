# Device providers

Embedding applications can supply additional devices through
`emulator_interfaces::devices` before constructing a Windows emulator. Each
`device_registration` owns its UTF-16 NT device name and supplies the existing
`device_factory`. Registrations are immutable after construction and apply only to
that emulator. The factory and open validator receive the registration's shared
owner through `device_creation_context::provider`, allowing the same factory to use
different state in different emulators. Empty names, null factories, duplicate
names and replacement of a built-in device are rejected before guest execution.

```cpp
sogen::emulator_interfaces interfaces{};
interfaces.devices.push_back({.name = u"Echo", .factory = create_echo, .dos_alias = true});
```

The NT name above is `\Device\Echo`. Setting `dos_alias` also recognizes
`\??\Echo`, the native path used for the corresponding DOS device open. Without
that opt-in, DOS paths keep their normal filesystem behavior. Existing built-in
aliases, console handles, NUL and named pipes keep their routes; reserved console
and named-pipe aliases cannot be registered. Name matching retains the existing
case-sensitive device lookup.

An optional `validate_open` function examines `device_creation_context` before an
NT open constructs the device. It returns the desired failure `NTSTATUS`, or
`STATUS_SUCCESS` to continue. This allows architecture restrictions without a
device-specific branch in the syscall handler. Direct construction of an
`io_device_container` is a host operation and does not perform this NT-open check.
A factory must return a device; returning null raises a host error.

The optional `lifetime` shared owner keeps provider resources alive. Both the
registry and every created shared device file state retain it. Duplicated handles
share the device and its owner. Device destruction precedes release of that file
state's owner; the registry can retain the provider until emulator destruction.
Providers must stop their host work before device destruction completes, and any
callback that can outlive a device needs its own ownership and cancellation policy.
This owner does not implement runtime loading or unloading.

The ordinary `io_device` interface still handles creation, work, IOCTLs, cancellation
and completion. Device implementations validate their own guest buffers and copied
payloads. They must not retain borrowed emulator state beyond its lifetime. An
external device always rejects snapshots while its handle remains open, even if its
own `supports_serialization` method returns true. Close all external handles before
capturing state. Built-in serialization and shared-handle reconstruction are unchanged.
No provider code pointers, paths or ownership objects are stored in snapshots.

Applications restoring a device container separately from the full emulator must
register a `windows_emulator_wrapper` factory with their buffer deserializer, in
addition to any context required by the underlying device. Full emulator restore
already supplies this factory.

This is a source interface for providers linked with their embedding application.
It does not introduce a stable binary ABI or make the stock analyzer load modules.
Browser applications link providers into their worker at build time using the same
registration interface.

## Example

`examples/device-provider` builds a separate embedding executable with the other
standalone targets, linking an echo device with the ordinary emulator libraries. It
exchanges a copied integer through the device completion path without loading a
guest executable or registry hives. Configure the emulator with standalone tools
enabled, then build the example explicitly:

```sh
cmake --build /path/to/emulator-build --target device-provider-example
mkdir -p /tmp/device-provider-root/filesys
/path/to/emulator-build/artifacts/device-provider-example /tmp/device-provider-root
```

Use the same compiler, headers and configuration for the provider and emulator.
