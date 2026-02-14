import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import {
  Shield,
  Cpu,
  Terminal,
  ExternalLink,
  Github,
  Play,
  ArrowRight,
  BookOpen,
  Download,
  Lock,
  Bug,
  Split,
  Layers,
} from "lucide-react";
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
            <span className="flex-1">Try Online</span>
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
            <Github className="mr-2 h-5 w-5 group-hover:scale-110 transition-transform" />
            <span className="flex-1">Get Source</span>
            <ExternalLink className="ml-2 h-4 w-4" />
          </span>
        </Button>
      </a>
    </div>
  );
}

export function LandingPage() {
  const features = [
    {
      icon: <Cpu className="h-6 w-6" />,
      title: "Syscall Emulation",
      description:
        "Operates at syscall level for Windows NT and Linux x86-64 binaries, minimizing API reimplementation overhead",
      accent: "from-[#f76548] to-[#b00101]",
    },
    {
      icon: <Split className="h-6 w-6" />,
      title: "Hooking Capabilities",
      description:
        "Provides powerful hooking interfaces to intercept memory access, code execution and much more",
      accent: "from-[#ffcb00] to-[#da6000]",
    },
    {
      icon: <Terminal className="h-6 w-6" />,
      title: "Debugging Interface",
      description:
        "Implements GDB serial protocol for integration with common debugging tools",
      accent: "from-[#00c4e9] to-[#005ff6]",
    },
    {
      icon: <Layers className="h-6 w-6" />,
      title: "State Management",
      description:
        "Saves and restores the entire state of the emulator to quickly resume your work exactly where you left off.",
      accent: "from-[#aee703] to-[#647502]",
    },
  ];

  const useCases = [
    {
      icon: <Shield className="h-6 w-6" />,
      title: "Security Research",
      description:
        "Analyze security vulnerabilities in a controlled environment",
    },
    {
      icon: <Lock className="h-6 w-6" />,
      title: "DRM Research",
      description:
        "Study digital rights management systems and protection mechanisms",
    },
    {
      icon: <Bug className="h-6 w-6" />,
      title: "Malware Analysis",
      description:
        "Reverse engineer malicious software with full process control",
    },
  ];

  const stats = [
    { value: "100%", label: "Open Source" },
    { value: "14", label: "Platforms" },
    { value: "2", label: "OS Targets" },
    { value: "2", label: "Backends" },
    { value: "100%", label: "Deterministic" },
  ];

  return (
    <>
      <Header
        title="Sogen"
        description="A high-performance Windows and Linux user space emulator."
      />
      <div className="flex flex-col min-h-screen bg-linear-to-br from-zinc-900 via-neutral-900 to-black overflow-x-hidden">
        {/* Hero Section with Animated Background */}
        <section className="relative overflow-visible">
          {/* Animated Background Elements */}
          <div className="absolute inset-0 container mx-auto">
            <div className="absolute top-20 left-10 w-72 h-72 bg-yellow-500/15 rounded-full blur-3xl"></div>
            <div className="absolute top-40 right-20 w-96 h-96 bg-lime-500/15 rounded-full blur-3xl"></div>
            <div className="absolute bottom-20 left-1/3 w-80 h-80 bg-cyan-500/15 rounded-full blur-3xl"></div>
          </div>

          <div className="relative container mx-auto min-h-dvh p-1 min-[340px]:p-4 flex items-center xl:min-h-0 xl:px-6 xl:py-32">
            <div className="text-center space-y-8 max-w-4xl mx-auto">
              {/* Main Headline */}
              <h1 className="text-5xl md:text-7xl font-bold text-white leading-tight">
                Sogen
              </h1>

              <p className="text-xl md:text-2xl text-neutral-300 font-light leading-relaxed">
                A high-performance Windows and Linux user space emulator.
              </p>

              {
                /* CTA Buttons */
                generateButtons("pt-8")
              }

              {/* Stats */}
              <div className="flex justify-center flex-col min-[400px]:flex-row gap-6 sm:gap-8 pt-12">
                {stats.map((stat, index) => (
                  <div key={index} className="text-center">
                    <div className="text-2xl font-bold text-white">
                      {stat.value}
                    </div>
                    <div className="text-sm text-neutral-400">{stat.label}</div>
                  </div>
                ))}
              </div>
            </div>
          </div>
        </section>

        {/* Features Section with Hover Effects */}
        <section className="py-24 relative">
          <div className="container mx-auto px-6">
            <div className="text-center mb-16">
              <h2 className="text-4xl md:text-5xl font-bold text-white mb-6">
                Powerful Features
              </h2>
              <p className="text-xl text-neutral-400 max-w-2xl mx-auto">
                Built from the ground up for performance and accuracy.
              </p>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-2 gap-8 lg:m-32">
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

        {/* Use Cases */}
        <section className="py-24 bg-neutral-800/40">
          <div className="container mx-auto px-6">
            <div className="text-center mb-16">
              <h2 className="text-4xl font-bold text-white mb-6">
                Perfect For Your Research
              </h2>
              <p className="text-xl text-neutral-400">
                Designed for researchers who need precise control over user-space
                process execution on Windows and Linux.
              </p>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-3 gap-8 max-w-4xl mx-auto">
              {useCases.map((useCase, index) => (
                <div
                  key={index}
                  className="text-center p-8 rounded-2xl bg-neutral-800/50 border border-neutral-700 hover:border-neutral-600 hover:bg-neutral-800/80 cursor-default transition-all duration-150 group"
                >
                  <div className="w-12 h-12 mx-auto mb-4 rounded-[0.625rem] bg-linear-to-br from-cyan-500 to-blue-500 p-3">
                    <div className="text-neutral-800">{useCase.icon}</div>
                  </div>
                  <h3 className="text-xl font-semibold text-white mb-3">
                    {useCase.title}
                  </h3>
                  <p className="text-neutral-400">{useCase.description}</p>
                </div>
              ))}
            </div>
          </div>
        </section>

        {/* Video Section with Modern Design */}
        <section className="py-24">
          <div className="container mx-auto px-6">
            <div className="text-center mb-16">
              <h2 className="text-4xl font-bold text-white mb-6">
                See Sogen in Action
              </h2>
              <p className="text-xl text-neutral-400 max-w-3xl mx-auto">
                Watch a comprehensive overview of the emulator's capabilities
                and discover how it can accelerate your research workflow.
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
                  title="Soure Code"
                  className="text-neutral-400 hover:text-blue-400 transition-colors p-2 rounded-lg hover:bg-neutral-800/50"
                >
                  <Github className="h-6 w-6" />
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
