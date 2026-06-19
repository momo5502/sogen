import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import {
  Cpu,
  ExternalLink,
  Play,
  ArrowRight,
  BookOpen,
  Bug,
  Split,
  Save,
  Code,
} from "lucide-react";
import { Highlight } from "prism-react-renderer";
import type { PrismTheme } from "prism-react-renderer";

import { Header } from "./Header";
import { YoutubeVideo } from "@/components/youtube-video";

function generateButtons(additionalClasses: string = "") {
  return (
    <div
      className={`flex flex-col sm:flex-row gap-4 justify-center items-stretch sm:items-center px-4 min-[340px]:px-16 ${additionalClasses}`}
    >
      <a href="#/playground">
        <Button
          asChild
          size="lg"
          className="rounded-lg bg-linear-to-br from-white to-neutral-300 text-neutral-900 border-0 px-8 py-6 text-lg font-semibold group transition-all duration-100 w-full flex"
        >
          <span>
            <Play className="mr-2 h-5 w-5 transition-transform" />
            <span className="flex-1 text-center">Try Online</span>
            <ArrowRight className="ml-2 h-5 w-5 group-hover:translate-x-1 transition-transform" />
          </span>
        </Button>
      </a>
      <a href="https://github.com/momo5502/sogen" target="_blank">
        <Button
          asChild
          size="lg"
          variant="outline"
          className="rounded-lg border-neutral-600 text-neutral-300 hover:bg-neutral-800/50 px-8 py-6 text-lg font-semibold group transition-all duration-300 w-full flex"
        >
          <span>
            <Code className="mr-2 h-5 w-5 group-hover:scale-110 transition-transform" />
            <span className="flex-1 text-center">Get Source</span>
            <ExternalLink className="ml-2 h-4 w-4" />
          </span>
        </Button>
      </a>
    </div>
  );
}

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

const landingPythonTheme: PrismTheme = {
  plain: {
    color: "#e6e6e6",
    backgroundColor: "transparent",
  },
  styles: [
    {
      types: ["comment"],
      style: { color: "#8a8a8a", fontStyle: "italic" },
    },
    {
      types: ["keyword", "builtin", "decorator", "important", "atrule"],
      style: { color: "#F3A71F" },
    },
    {
      types: ["function", "property", "namespace", "symbol"],
      style: { color: "#2AA8F5" },
    },
    {
      types: ["string"],
      style: { color: "#9ABB28" },
    },
    {
      types: ["number", "boolean"],
      style: { color: "#E25A48" },
    },
    {
      types: ["operator", "punctuation"],
      style: { color: "#9a9a9a" },
    },
    {
      types: ["class-name", "constant"],
      style: { color: "#2AA8F5" },
    },
  ],
};

export function LandingPage() {
  const features = [
    {
      icon: <Cpu className="h-6 w-6" />,
      title: "Real DLLs, Not Stubs",
      description:
        "The actual ntdll, kernel32 and user32 execute inside the emulator. Nothing is reimplemented, so even undocumented quirks behave exactly like they do on the real OS.",
      accent: "from-[#f76548] to-[#b00101]",
    },
    {
      icon: <Split className="h-6 w-6" />,
      title: "Hook & Rewrite Everything",
      description:
        "Intercept memory reads and writes, individual instructions, syscalls and API calls, at any granularity. Observe what a program does, or change it on the fly.",
      accent: "from-[#ffcb00] to-[#da6000]",
    },
    {
      icon: <Save className="h-6 w-6" />,
      title: "Snapshot & Restore",
      description:
        "Serialize the entire emulator state, load minidumps, and jump back to any point. Re-examine the exact moment a bug happens, again and again, without replaying the run from the start.",
      accent: "from-[#aee703] to-[#647502]",
    },
  ];

  return (
    <>
      <Header
        title="Sogen - Windows & Linux Userspace Emulator"
        description="Sogen is a high-performance Windows & Linux userspace emulator. It runs binaries at the CPU and syscall level, letting you hook and inspect every instruction, memory access and API call. Ideal for security, malware and DRM research."
      />
      <div className="flex flex-col min-h-screen bg-linear-to-br from-zinc-900 via-neutral-900 to-black overflow-x-hidden">
        {/* Hero Section with Animated Background */}
        <section className="relative overflow-visible">
          <div className="relative container mx-auto px-4 min-[340px]:px-6 pt-28 pb-16 xl:pt-32 xl:pb-24">
            <div className="grid grid-cols-1 lg:grid-cols-2 gap-x-0 gap-y-12 items-center">
              {/* Text column */}
              <div className="text-center lg:text-left space-y-8 max-w-2xl mx-auto lg:mx-0">
                <h1 className="text-4xl md:text-5xl lg:text-4xl font-bold text-white leading-[1.2] tracking-tight">
                  Run any Windows binary.
                  <br />
                  <span className="text-neutral-300">Without Windows.</span>
                </h1>

                <p className="text-lg md:text-xl text-neutral-400 font-light leading-relaxed max-w-xl mx-auto lg:mx-0 text-balance">
                  Sogen is a userspace emulator for Windows and Linux binaries.
                  Hook, debug and snapshot any process, with control over every
                  instruction, syscall and API call.
                </p>

                {
                  /* CTA Buttons */
                  generateButtons("pt-2 lg:px-0 lg:justify-start")
                }
              </div>

              {/* Product shot */}
              <div className="relative">
                <div className="absolute -inset-4 bg-linear-to-r from-yellow-500/10 via-lime-500/10 to-cyan-500/10 rounded-3xl blur-2xl"></div>
                <img
                  src="https://momo5502.com/sogen/preview.svg"
                  alt="The Sogen emulator tracing a program's execution"
                  className="relative w-full"
                />
              </div>
            </div>
          </div>
        </section>

        {/* Features Section */}
        <section className="py-24 relative">
          <div className="container mx-auto px-6">
            <div className="text-center mb-16">
              <h2 className="text-4xl font-bold text-white mb-6">
                Everything You Need to Dig In
              </h2>
              <p className="text-xl text-neutral-400 max-w-2xl mx-auto">
                Built in C++ for accuracy and speed.
              </p>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-8 max-w-6xl mx-auto">
              {features.map((feature, index) => (
                <Card
                  key={index}
                  className="bg-neutral-800/50 border-neutral-700 hover:border-neutral-600 hover:bg-neutral-800/80 cursor-default transition-all duration-150 group hover:shadow-2xl"
                >
                  <CardHeader>
                    <div
                      className={`w-12 h-12 rounded-[0.625rem] bg-linear-to-br ${feature.accent} p-3 mb-4`}
                    >
                      <div className="text-neutral-900">{feature.icon}</div>
                    </div>
                    <CardTitle className="text-white text-xl font-semibold transition-colors">
                      {feature.title}
                    </CardTitle>
                  </CardHeader>
                  <CardContent>
                    <p className="text-neutral-300 leading-relaxed">
                      {feature.description}
                    </p>
                  </CardContent>
                </Card>
              ))}
            </div>
          </div>
        </section>

        {/* Runs Everywhere Strip */}
        <section className="py-16 border-y border-neutral-800/80">
          <div className="container mx-auto px-6 text-center">
            <p className="text-xs font-semibold uppercase tracking-[0.25em] text-neutral-500 mb-6">
              Runs everywhere
            </p>
            <div className="flex flex-wrap justify-center items-center gap-x-8 gap-y-3 text-lg md:text-xl font-medium text-neutral-200">
              <span>Windows</span>
              <span>Linux</span>
              <span>macOS</span>
              <span>Android</span>
              <span>iOS</span>
              <span>Browser</span>
            </div>
            <div className="mt-5 flex flex-wrap justify-center items-center gap-x-3 gap-y-2 text-sm text-neutral-500">
              <span>x86-64 &amp; arm64</span>
              <span className="text-neutral-700">·</span>
              <span>Unicorn / icicle / Hyper-V</span>
              <span className="text-neutral-700">·</span>
              <span>Deterministic</span>
            </div>
          </div>
        </section>

        {/* Debugger Showcase */}
        <section className="py-24 bg-linear-to-b from-neutral-900/0 to-neutral-800/40">
          <div className="container mx-auto px-6">
            <div className="grid grid-cols-1 xl:grid-cols-2 gap-12 items-center max-w-6xl mx-auto">
              <div className="relative order-last xl:order-first w-full max-w-2xl mx-auto">
                <div className="absolute -inset-4 bg-linear-to-r from-neutral-500/10 to-neutral-400/10 rounded-2xl blur-md"></div>
                <img
                  src="https://momo5502.com/sogen/debugger.png"
                  alt="An IDA Pro remote GDB session debugging a process running in Sogen"
                  width={1464}
                  height={902}
                  className="relative w-full rounded-xl border border-neutral-700 shadow-2xl"
                />
              </div>

              <div>
                <div className="inline-flex items-center gap-2 rounded-full border border-neutral-700 bg-neutral-800/60 px-4 py-2 text-sm text-neutral-300 mb-6">
                  <Bug className="h-4 w-4" />
                  Debugging
                </div>
                <h2 className="text-4xl font-bold text-white mb-6">
                  Debug It With the Tools You Trust
                </h2>
                <p className="text-xl text-neutral-400 leading-relaxed">
                  Sogen speaks the GDB protocol, so you can debug with tools you
                  already know, like IDA Pro, GDB, or right in the browser.
                  <br />
                  Since the debugger lives outside the process, anti-debugging
                  tricks have nothing to detect.
                </p>
              </div>
            </div>
          </div>
        </section>

        {/* Frontier / Experimental Section */}
        <section className="py-24 bg-linear-to-b from-neutral-900/0 to-neutral-800/40">
          <div className="container mx-auto px-6">
            <div className="grid grid-cols-1 xl:grid-cols-2 gap-12 items-center max-w-6xl mx-auto">
              <div>
                <div className="inline-flex items-center gap-2 rounded-full border border-neutral-700 bg-neutral-800/60 px-4 py-2 text-sm text-neutral-300 mb-6">
                  <span className="relative flex h-2 w-2">
                    <span className="absolute inline-flex h-full w-full animate-ping rounded-full bg-cyan-400 opacity-75"></span>
                    <span className="relative inline-flex h-2 w-2 rounded-full bg-cyan-500"></span>
                  </span>
                  Experimental
                </div>
                <h2 className="text-4xl font-bold text-white mb-6">
                  It Runs Real GUI Apps, Even Games
                </h2>
                <p className="text-xl text-neutral-400 leading-relaxed">
                  Real user32 windows, dialogs and controls run their own logic
                  and paint through emulated GDI, while a Vulkan bridge forwards
                  graphics to your host's real GPU. Enough to bring interactive
                  Windows applications, and even games, to life inside the
                  emulator.
                </p>
              </div>

              <div className="relative w-full max-w-2xl mx-auto">
                <div className="absolute -inset-4 bg-linear-to-r from-[#76b900]/10 to-cyan-500/10 rounded-2xl blur-md"></div>
                <img
                  src="https://momo5502.com/sogen/game.png"
                  alt="A game running inside the Sogen emulator"
                  width={1283}
                  height={754}
                  className="relative w-full rounded-xl border border-neutral-700 shadow-2xl"
                />
              </div>
            </div>
          </div>
        </section>

        {/* Python Bindings Section */}
        <section className="py-24 bg-linear-to-b from-neutral-900/0 to-neutral-800/40">
          <div className="container mx-auto px-6">
            <div className="grid grid-cols-1 xl:grid-cols-2 gap-12 items-center max-w-6xl mx-auto">
              <div>
                <div className="inline-flex items-center gap-2 rounded-full border border-neutral-700 bg-neutral-800/60 px-4 py-2 text-sm text-neutral-300 mb-6">
                  <BookOpen className="h-4 w-4" />
                  Python bindings
                </div>
                <h2 className="text-4xl font-bold text-white mb-6">
                  Automate Sogen from Python
                </h2>
                <p className="text-xl text-neutral-400 leading-relaxed mb-6">
                  Script emulator runs, register callbacks, and intercept WinAPI
                  calls directly from Python. Install from PyPI, load your
                  target, and start exploring.
                </p>

                <div className="mb-8 inline-flex items-center rounded-lg border border-neutral-700 bg-neutral-900/80 px-4 py-3 font-mono text-sm text-neutral-200">
                  <span className="text-neutral-500 mr-3 select-none">$</span>
                  <span className="select-all">pip install sogen</span>
                </div>

                <div className="flex flex-col sm:flex-row gap-4">
                  <a href="https://pypi.org/project/sogen/" target="_blank">
                    <Button
                      asChild
                      size="lg"
                      className="rounded-lg bg-linear-to-br from-white to-neutral-300 text-neutral-900 border-0 px-8 py-6 text-lg font-semibold group transition-all duration-100 w-full flex"
                    >
                      <span>
                        <BookOpen className="mr-2 h-5 w-5 transition-transform" />
                        <span className="flex-1 text-center">View on PyPI</span>
                        <ExternalLink className="ml-2 h-4 w-4" />
                      </span>
                    </Button>
                  </a>
                  <a
                    href="https://github.com/momo5502/sogen/blob/main/docs/python/README.md"
                    target="_blank"
                  >
                    <Button
                      asChild
                      size="lg"
                      variant="outline"
                      className="rounded-lg border-neutral-600 text-neutral-300 hover:bg-neutral-800/50 px-8 py-6 text-lg font-semibold group transition-all duration-300 w-full flex"
                    >
                      <span>
                        <Code className="mr-2 h-5 w-5 group-hover:scale-110 transition-transform" />
                        <span className="flex-1 text-center">
                          Read Python Docs
                        </span>
                        <ExternalLink className="ml-2 h-4 w-4" />
                      </span>
                    </Button>
                  </a>
                </div>
              </div>

              <div className="relative">
                <div className="absolute -inset-4 bg-linear-to-r from-yellow-500/10 to-cyan-500/10 rounded-2xl blur-md"></div>
                <div className="relative rounded-2xl border border-neutral-700 bg-neutral-900/90 overflow-hidden shadow-2xl">
                  <div className="flex items-center gap-2 border-b border-neutral-800 px-5 py-4 text-sm text-neutral-500">
                    <div className="h-3 w-3 rounded-full bg-red-400/80"></div>
                    <div className="h-3 w-3 rounded-full bg-yellow-400/80"></div>
                    <div className="h-3 w-3 rounded-full bg-green-400/80"></div>
                    <span className="ml-3">api_hooks.py</span>
                  </div>
                  <Highlight
                    theme={landingPythonTheme}
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
                </div>
              </div>
            </div>
          </div>
        </section>

        {/* Video Section */}
        <section className="py-24">
          <div className="container mx-auto px-6">
            <div className="text-center mb-16">
              <h2 className="text-4xl font-bold text-white mb-6">
                See Sogen in Action
              </h2>
              <p className="text-xl text-neutral-400 max-w-3xl mx-auto">
                Two walkthroughs of how Sogen works and what it can do.
              </p>
            </div>

            <div className="mx-auto w-full gap-12 flex items-center justify-center flex-col lg:flex-row">
              {["wY9Q0DhodOQ", "RkodCUEmiuA"].map((id) => {
                return (
                  <div
                    key={`video-${id}`}
                    className="flex-1 w-full max-w-xl relative group"
                  >
                    <div className="absolute -inset-4 bg-linear-to-r from-neutral-500/15 to-neutral-500/15 rounded-3xl blur-md group-hover:blur-lg transition-all duration-300"></div>
                    <div className="relative aspect-video rounded-2xl overflow-hidden ">
                      <YoutubeVideo id={id} />
                    </div>
                  </div>
                );
              })}
            </div>
          </div>
        </section>

        {/* CTA Section */}
        <section className="py-24 bg-linear-to-r from-neutral-800/40 to-neutral-900">
          <div className="container mx-auto px-6 text-center">
            <h2 className="text-4xl font-bold text-white mb-6">
              Ready to Start Emulating?
            </h2>
            <p className="text-xl text-neutral-300 mb-8 max-w-2xl mx-auto">
              Try Sogen directly in your browser or explore the source code.
            </p>
            {generateButtons()}
          </div>
        </section>

        {/* Footer */}
        <footer className="py-16 border-t border-neutral-800">
          <div className="container mx-auto px-6">
            <div className="flex flex-col md:flex-row justify-between items-center">
              <div className="mb-8 md:mb-0 text-center md:text-left">
                <h2 className="text-3xl font-bold">Sogen</h2>
                <p className="mt-1 text-neutral-500 text-sm">
                  Built by{" "}
                  <a
                    href="https://momo5502.com"
                    className="underline"
                    target="_blank"
                  >
                    momo5502
                  </a>{" "}
                  with lots of help from{" "}
                  <a
                    href="https://github.com/momo5502/sogen/graphs/contributors"
                    className="underline"
                    target="_blank"
                  >
                    the community
                  </a>
                  .
                </p>
              </div>
              <div className="flex items-center space-x-6">
                <a
                  href="https://github.com/momo5502/sogen"
                  target="_blank"
                  title="Source Code"
                  className="text-neutral-400 hover:text-blue-400 transition-colors p-2 rounded-lg hover:bg-neutral-800/50"
                >
                  <Code className="h-6 w-6" />
                </a>
                <a
                  href="#/playground"
                  title="Playground"
                  className="text-neutral-400 hover:text-blue-400 transition-colors p-2 rounded-lg hover:bg-neutral-800/50"
                >
                  <Play className="h-6 w-6" />
                </a>
                <a
                  href="https://github.com/momo5502/sogen/wiki"
                  target="_blank"
                  title="Wiki"
                  className="text-neutral-400 hover:text-blue-400 transition-colors p-2 rounded-lg hover:bg-neutral-800/50"
                >
                  <BookOpen className="h-6 w-6" />
                </a>
              </div>
            </div>
          </div>
        </footer>
      </div>
    </>
  );
}
