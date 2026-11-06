# Host-window event subscriptions

A device that creates a host window can receive its UI events without creating a guest
USER window. Keep the subscription alive for as long as the consumer can receive events:

```cpp
auto events = emulator.subscribe_host_window_events(window_id, [&](const ui_event& event) {
    input_queue.push_back(event);
});
```

The caller owns the host window and its nonzero ID. The ID must be distinct from guest
USER handles and other live host windows. An empty handler, a duplicate subscription or
an existing guest window is rejected. Do not reuse an ID for a different window while
the backend can still deliver events from the previous window.

Matching events reach the handler under the emulator kernel lock, before guest low-level
input hooks, key-state updates and USER message routing. The handler should copy the event
into its own queue and return promptly. It must not call an entry point that reacquires the
kernel lock, or wait for another thread that is unregistering a subscription. Unregistered
windows continue through the existing guest input path.

Subscriptions are move-only. Destroying one or calling `reset()` unregisters its handler
and waits for a callback running on another thread. A handler can unregister itself; its
callable remains alive until the current invocation returns. The consumer must release
the subscription before destroying data referenced by its handler. A subscription may
outlive the emulator and can safely be reset after that emulator is destroyed.

Snapshot restoration and deserialization invalidate host subscriptions before restoring
device and UI state. Host callbacks are not serialized. Recreate the window and subscribe
again after restoration. An old subscription cannot remove a newer registration for the
same ID. Ordinary snapshot behavior is unchanged when no host subscription exists.
