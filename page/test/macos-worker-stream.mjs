// The macOS event stream's contract, tested against the shipping worker rather than a stand-in.
//
// src/macos-web/main.cpp writes one JSON object per line on stdout and page/public/macos-emulator-worker
// splits, parses and routes them; page/src/macos/trace-view.tsx renders whatever comes out. Everything
// between the emulator and the trace therefore depends on two things this checks: that a line which
// parses reaches the page exactly once, as an event, and that a line which does not is still delivered
// as text rather than dropped or thrown on. A guest string reaches that stream verbatim -- a path, an
// argv, a log message -- so the escaping main.cpp applies has to survive the round trip intact.
//
// The worker is a classic worker script and exports nothing, so it is evaluated with its own source and
// asked for the two functions under test. That is deliberate: a reimplementation here would pass while
// the file that ships was broken.
//
// Usage: npm run test:worker-stream (from page/), or node test/macos-worker-stream.mjs directly.

import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WORKER = path.join(HERE, "..", "public", "macos-emulator-worker.js");

const sent = [];

// Set before the worker is evaluated: it captures self.postMessage at module scope
// (`const nativePostMessage = self.postMessage.bind(self)`) and then replaces the global with its own
// router, so only a recorder installed first sees what actually leaves the worker.
globalThis.self = globalThis;
globalThis.postMessage = (payload) => sent.push(payload);
// The file is strict-mode, and its top-level `onmessage = ...` would throw on an undeclared global.
globalThis.onmessage = null;

const source = await readFile(WORKER, "utf8");
const { emit, post } = new Function(`${source}\nreturn { emit, post };`)();

let failures = 0;

function check(ok, what, detail) {
  if (!ok) ++failures;
  console.log(`${ok ? "PASS" : "FAIL"}  ${what}`);
  if (!ok && detail !== undefined)
    console.log(`      got: ${JSON.stringify(detail)}`);
}

function drain() {
  const taken = sent.slice();
  sent.length = 0;
  return taken;
}

const logs = (messages) => messages.filter((m) => m.message === "log");
const events = (messages) =>
  messages.filter((m) => m.message === "macos-status");

/* ------------------------------------------------------------------ a parsed event */

emit('{"t":"syscall","name":"open","id":5,"pc":"0x1000002e4"}', false);
{
  const out = drain();
  check(
    events(out).length === 1,
    "a JSON line reaches the page once, as an event",
  );
  check(logs(out).length === 0, "and not a second time as terminal text", out);
  check(
    events(out)[0]?.event?.name === "open",
    "with its fields intact",
    events(out)[0],
  );
}

/* ------------------------------------------------------- a line that is not an event */

emit(
  "the served root at http://x/macos-root is answering what the page did not attach",
  false,
);
{
  const out = drain();
  check(
    logs(out).length === 1,
    "a plain line still reaches the Output terminal",
  );
  const event = events(out)[0]?.event;
  check(
    event?.t === "line" && event.error === 0,
    "and the trace, as a line row",
    event,
  );
}

emit("could not attach /root/x: Error", true);
{
  const event = events(drain())[0]?.event;
  check(
    event?.t === "line" && event.error === 1,
    "a failing line is flagged as one",
    event,
  );
}

/* ------------------------------------- a line that looks like JSON but does not parse */

emit("{not json at all}", false);
{
  const out = drain();
  const event = events(out)[0]?.event;
  check(
    event?.t === "line" && event.text === "{not json at all}",
    "an unparseable brace line falls back to text rather than vanishing",
    event,
  );
  check(logs(out).length === 1, "and still reaches the terminal");
}

emit('["not","an","object"]', false);
{
  const event = events(drain())[0]?.event;
  check(event?.t === "line", "a JSON array is text, not an event", event);
}

/* ------------------------------------------------------------------------ splitting */

emit('line one\n{"t":"stdout","text":"two"}\nline three', false);
{
  const out = drain();
  check(
    events(out).length === 3,
    "one write carrying newlines becomes one row each",
    events(out).length,
  );
  check(
    events(out)[1]?.event?.t === "stdout",
    "and each is routed on its own merits",
    events(out)[1],
  );
}

emit("", false);
emit("   ", false);
check(drain().length === 0, "a blank line produces nothing");

/* ------------------------------------------------------------- escaping, round trip */

// Exactly what src/macos-web/main.cpp's json_escape() produces for these bytes: a quote, a backslash,
// a newline, a tab, a carriage return, and two control characters as \u00XX. A guest path or an argv
// reaches that function verbatim, so this is the shape a hostile filename arrives in.
const hostile = 'he said "hi"\\ then\n\ttabbed\rbelldel';
emit(
  '{"t":"detail","label":"path","value":"he said \\"hi\\"\\\\ then\\n\\ttabbed\\r\\u0007bell\\u007fdel"}',
  false,
);
{
  const event = events(drain())[0]?.event;
  check(
    event?.value === hostile,
    "every escape json_escape emits survives the round trip",
    event?.value,
  );
}

// The line the whole feature exists to make visible, byte for byte as the module writes it.
emit(
  '{"t":"log","level":"info","text":"window 1 takes the shape (0, 0, 320, 232)"}',
  false,
);
{
  const event = events(drain())[0]?.event;
  check(
    event?.t === "log" &&
      event.text === "window 1 takes the shape (0, 0, 320, 232)",
    'the "window N takes the shape" line arrives as a log event',
    event,
  );
}

/* --------------------------------------------------------------------- bridge errors */

post({
  t: "bridge-error",
  path: "/root/x",
  offset: 4096,
  size: 16,
  message: "HTTP 500",
});
{
  const out = drain();
  check(logs(out).length === 1, "a range-bridge failure reaches the terminal");
  const event = events(out)[0]?.event;
  check(
    event?.t === "line" && event.error === 1 && event.text.includes("HTTP 500"),
    "and the trace, as a failing line",
    event,
  );
}

/* -------------------------------------------------------------------- run completion */

post({ t: "done", code: 3 });
{
  const out = drain();
  check(
    out.some((m) => m.message === "event") &&
      out.some((m) => m.message === "end" && m.data === 3),
    "a finished run still reports its exit status both ways",
    out.map((m) => m.message),
  );
}

console.log(failures ? `\n${failures} FAILED` : "\nall checks passed");
process.exit(failures ? 1 : 0);
