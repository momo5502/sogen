import {
  Bug,
  Lock,
  Box,
  ArrowRight,
  ArrowUpRight,
  BookOpen,
  Play,
} from "lucide-react";
import { Github } from "react-bootstrap-icons";
import type { ReactNode } from "react";
import { Highlight } from "prism-react-renderer";
import type { PrismTheme } from "prism-react-renderer";

import { Header } from "./Header";
import { YoutubeVideo } from "@/components/youtube-video";

import "./landing.css";

const pythonBindingsSample = `import ctypes
import sogen

app = sogen.windows.create_application(
    "c:/test-sample.exe", emulation_root="./root")

@sogen.windows.api_call(cc=sogen.CallingConvention.stdcall,
                        params=[ctypes.c_uint32])
def on_sleep(call, params):
    print(f"Sleep({params[0]})")

app.hooks.apis["Sleep"] = on_sleep
app.start()`;

const pythonTheme: PrismTheme = {
  plain: {
    color: "#e6e6e6",
    backgroundColor: "transparent",
  },
  styles: [
    { types: ["comment"], style: { color: "#8a8a8a", fontStyle: "italic" } },
    {
      types: ["keyword", "builtin", "decorator", "important", "atrule"],
      style: { color: "#f4c04a" },
    },
    {
      types: ["function", "property", "namespace", "symbol"],
      style: { color: "#5cb8f7" },
    },
    { types: ["string"], style: { color: "#9fd24a" } },
    { types: ["number", "boolean"], style: { color: "#e25a48" } },
    { types: ["operator", "punctuation"], style: { color: "#9a9a9a" } },
    { types: ["class-name", "constant"], style: { color: "#45c2cf" } },
  ],
};

/* The color ramp used by Sogen's terminal output, reused as the
   accent system across the page. */
const ramp = ["#f4c04a", "#cdd24a", "#9fd24a", "#5ecb9a", "#45c2cf", "#5cb8f7"];

function scrollTo(id: string) {
  document.getElementById(id)?.scrollIntoView({ behavior: "smooth" });
}

function Eyebrow({
  color,
  index,
  children,
}: {
  color: string;
  index?: string;
  children: ReactNode;
}) {
  return (
    <span className="inline-flex items-center gap-2.5 font-mono text-[0.6875rem] font-medium uppercase tracking-[0.18em] text-[var(--lp-ink-soft)]">
      <span
        aria-hidden
        className="inline-block h-2 w-2"
        style={{ backgroundColor: color }}
      />
      {index && <span aria-hidden>{index} /</span>}
      {children}
    </span>
  );
}

function PillLink({
  href,
  variant = "primary",
  external = false,
  children,
}: {
  href: string;
  variant?: "primary" | "ghost";
  external?: boolean;
  children: ReactNode;
}) {
  const base =
    "group inline-flex min-h-[44px] cursor-pointer items-center justify-center gap-2 rounded-full px-6 py-3 text-[0.9375rem] font-semibold transition-colors duration-200 focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-[var(--lp-accent)]";
  const styles =
    variant === "primary"
      ? "bg-[var(--lp-ink)] text-[var(--lp-paper)] hover:bg-black"
      : "border border-[var(--lp-hairline)] text-[var(--lp-ink)] hover:border-[rgba(27,26,23,0.35)] hover:bg-[rgba(27,26,23,0.03)]";

  return (
    <a
      href={href}
      target={external ? "_blank" : undefined}
      rel={external ? "noreferrer" : undefined}
      className={`${base} ${styles}`}
    >
      {children}
    </a>
  );
}

function WindowFrame({
  title,
  children,
  className = "",
}: {
  title: string;
  children: ReactNode;
  className?: string;
}) {
  return (
    <div
      className={`overflow-hidden rounded-xl border border-[rgba(27,26,23,0.15)] bg-neutral-950 shadow-[0_24px_48px_-24px_rgba(27,26,23,0.4)] ${className}`}
    >
      <div className="flex items-center gap-1.5 border-b border-white/10 px-4 py-3">
        <span className="h-2.5 w-2.5 rounded-full bg-[#e25a48]/80" />
        <span className="h-2.5 w-2.5 rounded-full bg-[#f4c04a]/80" />
        <span className="h-2.5 w-2.5 rounded-full bg-[#9fd24a]/80" />
        <span className="ml-3 truncate font-mono text-xs text-neutral-500">
          {title}
        </span>
      </div>
      {children}
    </div>
  );
}

export function LandingPage() {
  const capabilities = [
    {
      key: "ntdll.dll",
      title: "Real system DLLs",
      description:
        "Sogen maps and runs the actual Windows system libraries: ntdll, kernel32, user32. Because the real code runs, behavior matches the real OS, edge cases and quirks included.",
    },
    {
      key: 'hooks.apis["Sleep"]',
      title: "Hook & rewrite",
      description:
        "Intercept any instruction, memory access, syscall or API call. Log it, change its result, or block it entirely, from C++ or Python.",
    },
    {
      key: "snapshot()",
      title: "Snapshot & restore",
      description:
        "Serialize the full emulator state to disk, take fast in-memory snapshots, or load a Windows minidump and continue executing from the moment it captured.",
    },
    {
      key: "gdb remote",
      title: "Undetectable debugging",
      description:
        "A built-in GDB protocol server lets IDA Pro, GDB or the in-browser debugger attach from outside the process, invisible to anti-debug checks.",
    },
    {
      key: "unicorn · icicle · whp · kvm",
      title: "Four CPU backends",
      description:
        "Software emulation (Unicorn, icicle) when you need instruction-level control; hardware virtualization (Hyper-V, KVM) when you need near-native speed.",
    },
    {
      key: "replay",
      title: "Deterministic execution",
      description:
        "Every run is reproducible, down to the instruction. A bug that happened once happens every time you go looking for it.",
    },
    {
      key: "gui · d3d · dxvk",
      title: "Windows, graphics, games",
      description:
        "GUI apps run with working windows, dialogs and controls. GPU paravirtualization plus DXVK brings Direct3D 8–11 titles to your real GPU. (Experimental.)",
    },
    {
      key: "wasm32",
      title: "One core, every platform",
      description:
        "The same C++ core runs on Windows, Linux, macOS, Android and iOS, and compiles to WebAssembly to run entirely in your browser.",
    },
  ];

  const useCases = [
    {
      icon: Bug,
      title: "Analyze malware",
      description:
        "Run a sample and watch every syscall, memory access and API call it makes, without any risk to your machine.",
    },
    {
      icon: Lock,
      title: "Understand DRM",
      description:
        "Follow licensing and protection logic step by step, with a debugger the protection cannot see.",
    },
    {
      icon: Box,
      title: "Sandbox apps & games",
      description:
        "Run untrusted software, even games, in full isolation, with working windows and GPU acceleration.",
    },
  ];

  const navLinks = [
    { label: "How it works", id: "how" },
    { label: "Capabilities", id: "capabilities" },
    { label: "Debugger", id: "debugger" },
    { label: "Python", id: "python" },
  ];

  return (
    <>
      <Header
        title="Sogen - Windows & Linux Userspace Emulator"
        description="Sogen is a high-performance Windows & Linux userspace emulator. It runs binaries at the CPU and syscall level, letting you hook and inspect every instruction, memory access and API call. Ideal for security, malware and DRM research."
      />

      <div className="lp min-h-screen overflow-x-hidden font-sans">
        {/* ── Navigation ─────────────────────────────────────────── */}
        <nav className="sticky top-0 z-50 border-b border-[var(--lp-hairline)] bg-[rgba(250,250,247,0.85)] backdrop-blur-md">
          <div className="mx-auto flex h-16 max-w-6xl items-center justify-between px-5 sm:px-8">
            <a
              href="#/"
              className="text-lg font-bold tracking-tight text-[var(--lp-ink)]"
            >
              Sogen
              <span className="ml-0.5 text-[var(--lp-accent)]">.</span>
            </a>

            <div className="hidden items-center gap-1 md:flex">
              {navLinks.map((link) => (
                <button
                  key={link.id}
                  onClick={() => scrollTo(link.id)}
                  className="cursor-pointer rounded-full px-4 py-2 text-sm font-medium text-[var(--lp-ink-soft)] transition-colors duration-200 hover:bg-[rgba(27,26,23,0.05)] hover:text-[var(--lp-ink)]"
                >
                  {link.label}
                </button>
              ))}
              <a
                href="https://github.com/momo5502/sogen/wiki"
                target="_blank"
                rel="noreferrer"
                className="rounded-full px-4 py-2 text-sm font-medium text-[var(--lp-ink-soft)] transition-colors duration-200 hover:bg-[rgba(27,26,23,0.05)] hover:text-[var(--lp-ink)]"
              >
                Docs
              </a>
            </div>

            <div className="flex items-center gap-2">
              <a
                href="https://github.com/momo5502/sogen"
                target="_blank"
                rel="noreferrer"
                aria-label="Sogen on GitHub"
                className="flex h-10 w-10 items-center justify-center rounded-full text-[var(--lp-ink-soft)] transition-colors duration-200 hover:bg-[rgba(27,26,23,0.05)] hover:text-[var(--lp-ink)]"
              >
                <Github className="h-5 w-5" />
              </a>
              <a
                href="#/playground"
                className="inline-flex min-h-[40px] items-center gap-1.5 rounded-full bg-[var(--lp-ink)] px-4 py-2 text-sm font-semibold text-[var(--lp-paper)] transition-colors duration-200 hover:bg-black sm:px-5"
              >
                Try online
                <ArrowRight className="h-4 w-4" />
              </a>
            </div>
          </div>
        </nav>

        {/* ── Hero ───────────────────────────────────────────────── */}
        <section className="relative">
          <div className="lp-dots pointer-events-none absolute inset-x-0 top-0 h-[560px]" />
          <div className="relative mx-auto max-w-6xl px-5 pt-20 pb-14 sm:px-8 sm:pt-28 sm:pb-20">
            <div className="mx-auto max-w-3xl text-center">
              <Eyebrow color="var(--lp-accent)">
                Windows · Linux · Userspace emulator
              </Eyebrow>

              <h1 className="mt-6 text-[2.5rem] leading-[1.08] font-bold tracking-[-0.03em] text-balance sm:text-6xl md:text-[4.25rem]">
                Run Windows binaries.
                <br />
                <span className="text-[var(--lp-ink-soft)]">
                  Without Windows.
                </span>
              </h1>

              <p className="mx-auto mt-6 max-w-xl text-lg leading-relaxed text-pretty text-[var(--lp-ink-soft)] sm:text-xl">
                Sogen runs Windows and Linux programs without a real operating
                system, and lets you see and control everything they do, down to
                the instruction.
              </p>

              <div className="mt-9 flex flex-col items-center justify-center gap-3 sm:flex-row">
                <PillLink href="#/playground">
                  <Play className="h-4 w-4" />
                  Try it in your browser
                  <ArrowRight className="h-4 w-4 transition-transform duration-200 group-hover:translate-x-0.5" />
                </PillLink>
                <PillLink
                  href="https://github.com/momo5502/sogen"
                  variant="ghost"
                  external
                >
                  <Github className="h-4 w-4" />
                  View source
                </PillLink>
              </div>

              <p className="mt-5 font-mono text-xs text-[var(--lp-ink-soft)]/80">
                Free & open source · GPL-2.0 · Nothing to install
              </p>
            </div>

            <div className="mx-auto mt-14 max-w-4xl sm:mt-20">
              {/* preview.svg ships its own window chrome, so no WindowFrame here */}
              <div className="overflow-hidden rounded-xl border border-[rgba(27,26,23,0.15)] bg-neutral-950 shadow-[0_24px_48px_-24px_rgba(27,26,23,0.4)]">
                <img
                  src="https://momo5502.com/sogen/preview.svg"
                  alt="The Sogen emulator tracing a program's execution"
                  className="w-full"
                  width={1200}
                  height={680}
                />
              </div>
            </div>
          </div>
        </section>

        {/* ── How it works ───────────────────────────────────────── */}
        <section
          id="how"
          className="scroll-mt-20 border-y border-[var(--lp-hairline)] bg-[var(--lp-paper-2)]"
        >
          <div className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
            <div className="max-w-2xl">
              <Eyebrow index="01" color={ramp[4]}>
                How it works
              </Eyebrow>
              <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
                Emulate the syscalls. Run everything else for real.
              </h2>
              <p className="mt-5 text-lg leading-relaxed text-[var(--lp-ink-soft)]">
                Instead of reimplementing thousands of Windows APIs, Sogen loads
                your binary together with the real system DLLs and runs them on
                an emulated CPU. Only the thin syscall layer at the very bottom
                is answered by Sogen itself.
              </p>
            </div>

            <div className="lp-corners mt-14">
              <div className="grid grid-cols-1 items-stretch gap-3 lg:grid-cols-[1fr_auto_1.5fr_auto_1fr] lg:gap-4">
                {/* Input */}
                <div className="flex flex-col rounded-lg border border-[var(--lp-hairline)] bg-[var(--lp-paper)] p-5">
                  <span className="font-mono text-[0.6875rem] tracking-[0.18em] text-[var(--lp-ink-soft)] uppercase">
                    Input
                  </span>
                  <span className="mt-2 font-mono text-sm font-semibold">
                    your-program.exe
                  </span>
                  <p className="mt-2 text-sm leading-relaxed text-[var(--lp-ink-soft)]">
                    An unmodified Windows or Linux binary. Nothing recompiled,
                    nothing patched.
                  </p>
                </div>

                <div
                  aria-hidden
                  className="flex items-center justify-center font-mono text-lg text-[var(--lp-ink-soft)]"
                >
                  <span className="rotate-90 lg:rotate-0">→</span>
                </div>

                {/* Sogen stack */}
                <div className="overflow-hidden rounded-lg border border-[rgba(27,26,23,0.4)] bg-[var(--lp-paper)]">
                  <div className="border-b border-[var(--lp-hairline)] px-5 py-3 font-mono text-[0.6875rem] font-semibold tracking-[0.18em] uppercase">
                    Sogen
                  </div>
                  <div className="divide-y divide-[var(--lp-hairline)]">
                    {[
                      [
                        ramp[0],
                        "Real system DLLs",
                        "ntdll · kernel32 · user32, the actual files, not stubs",
                      ],
                      [
                        ramp[2],
                        "Emulated CPU",
                        "Unicorn · icicle · Hyper-V · KVM, on x86-64 and arm64",
                      ],
                      [
                        ramp[5],
                        "Syscall layer",
                        "NT syscalls implemented by Sogen, the only reimplemented part",
                      ],
                    ].map(([color, title, sub]) => (
                      <div key={title} className="px-5 py-3.5">
                        <div className="flex items-center gap-2.5">
                          <span
                            aria-hidden
                            className="inline-block h-2 w-2"
                            style={{ backgroundColor: color }}
                          />
                          <span className="text-sm font-semibold">{title}</span>
                        </div>
                        <p className="mt-1 pl-[1.125rem] font-mono text-xs leading-relaxed text-[var(--lp-ink-soft)]">
                          {sub}
                        </p>
                      </div>
                    ))}
                  </div>
                  <div className="border-t border-dashed border-[rgba(27,26,23,0.25)] px-5 py-3 font-mono text-xs leading-relaxed text-[var(--lp-ink-soft)]">
                    ↳ your hooks attach to every instruction, memory access,
                    syscall and API call
                  </div>
                </div>

                <div
                  aria-hidden
                  className="flex items-center justify-center font-mono text-lg text-[var(--lp-ink-soft)]"
                >
                  <span className="rotate-90 lg:rotate-0">→</span>
                </div>

                {/* Output */}
                <div className="flex flex-col rounded-lg border border-[var(--lp-hairline)] bg-[var(--lp-paper)] p-5">
                  <span className="font-mono text-[0.6875rem] tracking-[0.18em] text-[var(--lp-ink-soft)] uppercase">
                    Environment
                  </span>
                  <span className="mt-2 font-mono text-sm font-semibold">
                    virtual filesystem · registry · network
                  </span>
                  <p className="mt-2 text-sm leading-relaxed text-[var(--lp-ink-soft)]">
                    The program sees its own emulated environment and runs in
                    full isolation. Nothing touches your machine.
                  </p>
                </div>
              </div>
            </div>

            <p className="mt-8 font-mono text-sm text-[var(--lp-ink-soft)]">
              Real DLLs on top, emulated syscalls below: behavior closely
              matches the real OS, edge cases included.
            </p>
          </div>
        </section>

        {/* ── Use cases ──────────────────────────────────────────── */}
        <section className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
          <div className="max-w-2xl">
            <Eyebrow index="02" color={ramp[0]}>
              What it's for
            </Eyebrow>
            <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
              A safe, fully instrumented place to run code you don't trust.
            </h2>
          </div>

          <div className="mt-12 grid grid-cols-1 gap-x-10 gap-y-10 border-t border-[var(--lp-hairline)] pt-10 sm:grid-cols-3">
            {useCases.map((useCase, i) => (
              <div key={useCase.title}>
                <div className="flex items-baseline gap-3 font-mono text-sm text-[var(--lp-ink-soft)]">
                  <span>0{i + 1}</span>
                  <useCase.icon
                    className="h-4 w-4 translate-y-0.5"
                    aria-hidden
                  />
                </div>
                <h3 className="mt-3 text-lg font-semibold">{useCase.title}</h3>
                <p className="mt-2 leading-relaxed text-[var(--lp-ink-soft)]">
                  {useCase.description}
                </p>
              </div>
            ))}
          </div>
        </section>

        {/* ── Capability sheet ───────────────────────────────────── */}
        <section
          id="capabilities"
          className="scroll-mt-20 border-y border-[var(--lp-hairline)] bg-[var(--lp-paper-2)]"
        >
          <div className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
            <div className="max-w-2xl">
              <Eyebrow index="03" color={ramp[2]}>
                Capabilities
              </Eyebrow>
              <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
                Everything it does, in one sheet.
              </h2>
            </div>

            <div className="mt-12 border-t border-[var(--lp-hairline)]">
              {capabilities.map((cap, i) => (
                <div
                  key={cap.title}
                  className="grid grid-cols-1 gap-2 border-b border-[var(--lp-hairline)] py-6 md:grid-cols-[16rem_1fr] md:gap-8"
                >
                  <div className="flex items-start gap-2.5 font-mono text-sm text-[var(--lp-ink)]">
                    <span
                      aria-hidden
                      className="mt-1.5 inline-block h-2 w-2 shrink-0"
                      style={{ backgroundColor: ramp[i % ramp.length] }}
                    />
                    <span className="break-all">{cap.key}</span>
                  </div>
                  <p className="leading-relaxed">
                    <span className="font-semibold">{cap.title}.</span>{" "}
                    <span className="text-[var(--lp-ink-soft)]">
                      {cap.description}
                    </span>
                  </p>
                </div>
              ))}
            </div>
          </div>
        </section>

        {/* ── Browser showcase ───────────────────────────────────── */}
        <section className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
          <div className="grid grid-cols-1 items-center gap-12 lg:grid-cols-2 lg:gap-16">
            <div>
              <Eyebrow index="04" color={ramp[4]}>
                In your browser
              </Eyebrow>
              <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
                The whole emulator, compiled to WebAssembly.
              </h2>
              <p className="mt-5 text-lg leading-relaxed text-[var(--lp-ink-soft)]">
                Sogen runs entirely in your browser. Nothing is uploaded,
                everything runs locally, and there is nothing to install. Drop
                in a binary and start tracing.
              </p>
              <div className="mt-8">
                <PillLink href="#/playground">
                  <Play className="h-4 w-4" />
                  Open the playground
                  <ArrowRight className="h-4 w-4 transition-transform duration-200 group-hover:translate-x-0.5" />
                </PillLink>
              </div>
            </div>

            <WindowFrame title="sogen.dev/playground">
              <img
                src="https://momo5502.com/sogen/browser.png"
                alt="The Sogen playground running in a web browser"
                width={1017}
                height={583}
                loading="lazy"
                className="w-full"
              />
            </WindowFrame>
          </div>
        </section>

        {/* ── Debugger showcase ──────────────────────────────────── */}
        <section
          id="debugger"
          className="scroll-mt-20 border-y border-[var(--lp-hairline)] bg-[var(--lp-paper-2)]"
        >
          <div className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
            <div className="grid grid-cols-1 items-center gap-12 lg:grid-cols-2 lg:gap-16">
              <WindowFrame
                title="IDA Pro - remote GDB session"
                className="order-last lg:order-first"
              >
                <img
                  src="https://momo5502.com/sogen/debugger.png"
                  alt="An IDA Pro remote GDB session debugging a process running in Sogen"
                  width={1464}
                  height={902}
                  loading="lazy"
                  className="w-full"
                />
              </WindowFrame>

              <div>
                <Eyebrow index="05" color={ramp[5]}>
                  Debugging
                </Eyebrow>
                <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
                  A debugger anti-debug checks can't see.
                </h2>
                <p className="mt-5 text-lg leading-relaxed text-[var(--lp-ink-soft)]">
                  Sogen implements the GDB protocol, so you can debug with tools
                  you already know, like IDA Pro or GDB, or use the built-in
                  in-browser debugger.
                </p>
                <p className="mt-4 text-lg leading-relaxed text-[var(--lp-ink-soft)]">
                  The debugger runs at the emulator level, outside the process,
                  so it stays invisible to anti-debug checks.
                </p>
              </div>
            </div>
          </div>
        </section>

        {/* ── Games showcase ─────────────────────────────────────── */}
        <section id="games" className="scroll-mt-20">
          <div className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
            <div className="grid grid-cols-1 items-center gap-12 lg:grid-cols-2 lg:gap-16">
              <div>
                <Eyebrow index="06" color={ramp[3]}>
                  Games · Experimental
                </Eyebrow>
                <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
                  Fast enough for games.
                </h2>
                <p className="mt-5 text-lg leading-relaxed text-[var(--lp-ink-soft)]">
                  Native GUI apps run, with working windows, dialogs and
                  controls. GPU paravirtualization enables 3D acceleration on
                  your real GPU, while the Hyper-V backend runs the code
                  natively on your CPU.
                </p>
                <p className="mt-4 text-lg leading-relaxed text-[var(--lp-ink-soft)]">
                  Direct3D 8–11 titles run through DXVK, which translates
                  Direct3D to Vulkan on top of the GPU bridge.
                </p>
              </div>

              <WindowFrame title="game.exe - sandboxed">
                <img
                  src="https://momo5502.com/sogen/game.png"
                  alt="A game running inside the Sogen emulator"
                  width={1283}
                  height={754}
                  loading="lazy"
                  className="w-full"
                />
              </WindowFrame>
            </div>
          </div>
        </section>

        {/* ── Python bindings ────────────────────────────────────── */}
        <section
          id="python"
          className="scroll-mt-20 border-y border-[var(--lp-hairline)] bg-[var(--lp-paper-2)]"
        >
          <div className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
            <div className="grid grid-cols-1 items-center gap-12 lg:grid-cols-2 lg:gap-16">
              <div>
                <Eyebrow index="07" color={ramp[1]}>
                  Python bindings
                </Eyebrow>
                <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
                  Script the emulator in Python.
                </h2>
                <p className="mt-5 text-lg leading-relaxed text-[var(--lp-ink-soft)]">
                  Drive Sogen from Python: register callbacks, hook API calls,
                  and read or write memory, all from a few lines of code.
                </p>

                <div className="mt-7 inline-flex items-center rounded-lg border border-[var(--lp-hairline)] bg-[var(--lp-paper)] px-4 py-3 font-mono text-sm">
                  <span className="mr-3 select-none text-[var(--lp-ink-soft)]">
                    $
                  </span>
                  <span className="select-all">pip install sogen</span>
                </div>

                <div className="mt-8 flex flex-col gap-3 sm:flex-row">
                  <PillLink href="https://pypi.org/project/sogen/" external>
                    <BookOpen className="h-4 w-4" />
                    View on PyPI
                    <ArrowUpRight className="h-4 w-4" />
                  </PillLink>
                  <PillLink
                    href="https://github.com/momo5502/sogen/blob/main/docs/python/README.md"
                    variant="ghost"
                    external
                  >
                    Read the docs
                    <ArrowUpRight className="h-4 w-4" />
                  </PillLink>
                </div>
              </div>

              <WindowFrame title="api_hooks.py">
                <Highlight
                  theme={pythonTheme}
                  code={pythonBindingsSample}
                  language="python"
                >
                  {({
                    className,
                    style,
                    tokens,
                    getLineProps,
                    getTokenProps,
                  }) => (
                    <pre
                      className={`${className} overflow-x-auto p-5 text-sm leading-7`}
                      style={{
                        ...style,
                        margin: 0,
                        backgroundColor: "transparent",
                      }}
                    >
                      {tokens.map((line, i) => (
                        <div key={i} {...getLineProps({ line })}>
                          {line.map((token, key) => (
                            <span key={key} {...getTokenProps({ token })} />
                          ))}
                        </div>
                      ))}
                    </pre>
                  )}
                </Highlight>
              </WindowFrame>
            </div>
          </div>
        </section>

        {/* ── Videos ─────────────────────────────────────────────── */}
        <section className="mx-auto max-w-6xl px-5 py-20 sm:px-8 sm:py-28">
          <div className="mx-auto max-w-2xl text-center">
            <Eyebrow index="08" color={ramp[5]}>
              Watch
            </Eyebrow>
            <h2 className="mt-4 text-3xl font-bold tracking-tight text-balance sm:text-4xl">
              See Sogen in action.
            </h2>
            <p className="mt-4 text-lg text-[var(--lp-ink-soft)]">
              Two walkthroughs of how Sogen works and what it can do.
            </p>
          </div>

          <div className="mt-12 grid grid-cols-1 gap-8 lg:grid-cols-2">
            {["wY9Q0DhodOQ", "RkodCUEmiuA"].map((id) => (
              <div
                key={id}
                className="aspect-video overflow-hidden rounded-xl border border-[rgba(27,26,23,0.15)] bg-neutral-950 shadow-[0_24px_48px_-24px_rgba(27,26,23,0.4)]"
              >
                <YoutubeVideo id={id} />
              </div>
            ))}
          </div>
        </section>

        {/* ── CTA panel ──────────────────────────────────────────── */}
        <section className="mx-auto max-w-6xl px-5 pb-20 sm:px-8 sm:pb-28">
          <div className="rounded-2xl bg-[var(--lp-ink)] px-6 py-16 text-center sm:px-12 sm:py-20">
            <span className="font-mono text-[0.6875rem] font-medium tracking-[0.18em] text-neutral-400 uppercase">
              Open source
            </span>
            <h2 className="mx-auto mt-4 max-w-xl text-3xl font-bold tracking-tight text-balance text-[var(--lp-paper)] sm:text-4xl">
              Help build Sogen.
            </h2>
            <p className="mx-auto mt-4 max-w-xl text-lg leading-relaxed text-neutral-400">
              There's always more to build. Report a bug, add a syscall, or open
              a pull request.
            </p>
            <div className="mt-9 flex flex-col items-center justify-center gap-3 sm:flex-row">
              <a
                href="https://github.com/momo5502/sogen"
                target="_blank"
                rel="noreferrer"
                className="group inline-flex min-h-[44px] cursor-pointer items-center justify-center gap-2 rounded-full bg-[var(--lp-paper)] px-6 py-3 text-[0.9375rem] font-semibold text-[var(--lp-ink)] transition-colors duration-200 hover:bg-white focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-[var(--lp-accent)]"
              >
                <Github className="h-4 w-4" />
                Contribute on GitHub
                <ArrowUpRight className="h-4 w-4" />
              </a>
              <a
                href="https://github.com/momo5502/sogen/wiki"
                target="_blank"
                rel="noreferrer"
                className="inline-flex min-h-[44px] cursor-pointer items-center justify-center gap-2 rounded-full border border-white/20 px-6 py-3 text-[0.9375rem] font-semibold text-[var(--lp-paper)] transition-colors duration-200 hover:border-white/40 hover:bg-white/5 focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-[var(--lp-accent)]"
              >
                <BookOpen className="h-4 w-4" />
                Read the wiki
              </a>
            </div>
          </div>
        </section>

        {/* ── Footer ─────────────────────────────────────────────── */}
        <footer className="border-t border-[var(--lp-hairline)]">
          <div className="mx-auto max-w-6xl px-5 py-14 sm:px-8">
            <div className="flex flex-col justify-between gap-10 sm:flex-row">
              <div className="max-w-xs">
                <div className="text-lg font-bold tracking-tight">
                  Sogen
                  <span className="ml-0.5 text-[var(--lp-accent)]">.</span>
                </div>
                <p className="mt-3 text-sm leading-relaxed text-[var(--lp-ink-soft)]">
                  Built by{" "}
                  <a
                    href="https://momo5502.com"
                    target="_blank"
                    rel="noreferrer"
                    className="underline underline-offset-2 transition-colors duration-200 hover:text-[var(--lp-ink)]"
                  >
                    momo5502
                  </a>{" "}
                  with lots of help from{" "}
                  <a
                    href="https://github.com/momo5502/sogen/graphs/contributors?all=1"
                    target="_blank"
                    rel="noreferrer"
                    className="underline underline-offset-2 transition-colors duration-200 hover:text-[var(--lp-ink)]"
                  >
                    the community
                  </a>
                  .
                </p>
              </div>

              <div className="grid grid-cols-2 gap-10 sm:gap-16">
                <div>
                  <div className="font-mono text-[0.6875rem] font-medium tracking-[0.18em] text-[var(--lp-ink-soft)] uppercase">
                    Project
                  </div>
                  <ul className="mt-4 space-y-2.5 text-sm">
                    {[
                      ["Playground", "#/playground", false],
                      ["GitHub", "https://github.com/momo5502/sogen", true],
                      ["Wiki", "https://github.com/momo5502/sogen/wiki", true],
                      [
                        "Issues",
                        "https://github.com/momo5502/sogen/issues",
                        true,
                      ],
                    ].map(([label, href, external]) => (
                      <li key={label as string}>
                        <a
                          href={href as string}
                          target={external ? "_blank" : undefined}
                          rel={external ? "noreferrer" : undefined}
                          className="text-[var(--lp-ink-soft)] transition-colors duration-200 hover:text-[var(--lp-ink)]"
                        >
                          {label}
                        </a>
                      </li>
                    ))}
                  </ul>
                </div>
                <div>
                  <div className="font-mono text-[0.6875rem] font-medium tracking-[0.18em] text-[var(--lp-ink-soft)] uppercase">
                    Resources
                  </div>
                  <ul className="mt-4 space-y-2.5 text-sm">
                    {[
                      ["PyPI", "https://pypi.org/project/sogen/"],
                      [
                        "Python docs",
                        "https://github.com/momo5502/sogen/blob/main/docs/python/README.md",
                      ],
                      [
                        "Overview video",
                        "https://www.youtube.com/watch?v=wY9Q0DhodOQ",
                      ],
                      [
                        "Slides",
                        "https://docs.google.com/presentation/d/1pha4tFfDMpVzJ_ehJJ21SA_HAWkufQBVYQvh1IFhVls/edit",
                      ],
                    ].map(([label, href]) => (
                      <li key={label}>
                        <a
                          href={href}
                          target="_blank"
                          rel="noreferrer"
                          className="text-[var(--lp-ink-soft)] transition-colors duration-200 hover:text-[var(--lp-ink)]"
                        >
                          {label}
                        </a>
                      </li>
                    ))}
                  </ul>
                </div>
              </div>
            </div>

            <div className="mt-12 flex flex-col items-start justify-between gap-3 border-t border-[var(--lp-hairline)] pt-6 font-mono text-xs text-[var(--lp-ink-soft)] sm:flex-row sm:items-center">
              <span>GPL-2.0 licensed</span>
              <span>Windows · Linux · macOS · Android · iOS · Web</span>
            </div>
          </div>
        </footer>
      </div>
    </>
  );
}
