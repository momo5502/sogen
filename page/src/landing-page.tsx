import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import {
  Shield,
  FileCode,
  Layers,
  Cpu,
  Database,
  Terminal,
  ExternalLink,
  Github,
  Play,
  ArrowRight,
  Code,
  Target,
  BookOpen,
  Download,
} from "lucide-react";
import { Header } from "./Header";

export function LandingPage() {
  const features = [
    {
      icon: <Cpu className="h-8 w-8" />,
      title: "Syscall-Level Emulation",
      description:
        "Operates at syscall level, leveraging existing system DLLs instead of reimplementing Windows APIs",
      accent: "from-cyan-500 to-blue-500",
    },
    {
      icon: <Database className="h-8 w-8" />,
      title: "Advanced Memory Management",
      description:
        "Supports Windows-specific memory types including reserved, committed, built on top of Unicorn's memory management",
      accent: "from-purple-500 to-pink-500",
    },
    {
      icon: <FileCode className="h-8 w-8" />,
      title: "Complete PE Loading",
      description:
        "Handles executable and DLL loading with proper memory mapping, relocations, and TLS",
      accent: "from-lime-400 to-green-500",
    },
    {
      icon: <Shield className="h-8 w-8" />,
      title: "Exception Handling",
      description:
        "Implements Windows structured exception handling (SEH) with proper exception dispatcher and unwinding support",
      accent: "from-orange-400 to-red-500",
    },
    {
      icon: <Layers className="h-8 w-8" />,
      title: "Threading Support",
      description: "Provides a scheduled (round-robin) threading model",
      accent: "from-teal-500 to-blue-500",
    },
    {
      icon: <Terminal className="h-8 w-8" />,
      title: "Debugging Interface",
      description:
        "Implements GDB serial protocol for integration with common debugging tools",
      accent: "from-purple-500 to-indigo-500",
    },
  ];

  const useCases = [
    {
      icon: <Shield className="h-6 w-6" />,
      title: "Security Research",
      description:
        "Analyze malware and security vulnerabilities in a controlled environment",
    },
    {
      icon: <Code className="h-6 w-6" />,
      title: "DRM Research",
      description:
        "Study digital rights management systems and protection mechanisms",
    },
    {
      icon: <Target className="h-6 w-6" />,
      title: "Malware Analysis",
      description:
        "Reverse engineer malicious software with full process control",
    },
  ];

  const stats = [
    { value: "100%", label: "Open Source" },
    { value: "C++", label: "High Performance" },
    { value: "GDB", label: "Debug Protocol" },
    { value: "64 bit", label: "Native PE Loading" },
  ];

  return (
    <>
      <Header
        title="Sogen - Windows User Space Emulator"
        description="Sogen is a high-performance Windows user space emulator that can emulate windows processes. It is ideal for security-, DRM- or malware research."
      />
      <div className="flex flex-col min-h-screen bg-gradient-to-br from-zinc-900 via-neutral-900 to-black">
        {/* Hero Section with Animated Background */}
        <section className="relative overflow-hidden">
          {/* Animated Background Elements */}
          <div className="absolute inset-0">
            <div className="absolute top-20 left-10 w-72 h-72 bg-blue-500/10 rounded-full blur-3xl animate-pulse"></div>
            <div className="absolute top-40 right-20 w-96 h-96 bg-purple-500/10 rounded-full blur-3xl animate-pulse delay-2000"></div>
            <div className="absolute bottom-20 left-1/3 w-80 h-80 bg-cyan-500/10 rounded-full blur-3xl animate-pulse delay-2000"></div>
          </div>

          <div className="relative container mx-auto min-h-[100dvh] p-4 flex items-center xl:min-h-0 xl:px-6 xl:py-32">
            <div className="text-center space-y-8 max-w-4xl mx-auto">
              {/* Main Headline */}
              <h1 className="text-5xl md:text-7xl font-bold bg-gradient-to-r from-white via-blue-100 to-cyan-200 bg-clip-text text-transparent leading-tight">
                Sogen
              </h1>

              <p className="text-xl md:text-2xl text-neutral-300 font-light leading-relaxed">
                A high-performance Windows user space emulator.
              </p>

              {/* CTA Buttons */}
              <div className="flex flex-col sm:flex-row gap-4 justify-center items-center pt-8">
                <a href="#/playground">
                  <Button
                    size="lg"
                    className="bg-gradient-to-r from-blue-600 to-cyan-600 hover:from-blue-700 hover:to-cyan-700 text-white border-0 px-8 py-6 text-lg font-semibold group transition-all duration-300 transform hover:scale-105"
                  >
                    <Play className="mr-2 h-5 w-5 group-hover:scale-110 transition-transform" />
                    Try Online
                    <ArrowRight className="ml-2 h-5 w-5 group-hover:translate-x-1 transition-transform" />
                  </Button>
                </a>
                <a href="https://github.com/momo5502/sogen" target="_blank">
                  <Button
                    size="lg"
                    variant="outline"
                    className="border-neutral-600 text-neutral-300 hover:bg-neutral-800/50 px-8 py-6 text-lg font-semibold group transition-all duration-300"
                  >
                    <Github className="mr-2 h-5 w-5 group-hover:scale-110 transition-transform" />
                    View Source
                    <ExternalLink className="ml-2 h-4 w-4" />
                  </Button>
                </a>
              </div>

              {/* Stats */}
              <div className="flex justify-center gap-8 pt-12">
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
                Built from the ground up for performance and accuracy in Windows
                process emulation
              </p>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-8">
              {features.map((feature, index) => (
                <Card
                  key={index}
                  className="bg-neutral-800/50 border-neutral-700 hover:bg-neutral-800/80 transition-all duration-200 group hover:shadow-2xl"
                >
                  <CardHeader className="pb-4">
                    <div
                      className={`w-16 h-16 rounded-xl bg-gradient-to-br ${feature.accent} p-4 mb-4 transition-transform duration-200`}
                    >
                      <div className="text-white">{feature.icon}</div>
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
                Designed for researchers who need precise control over Windows
                process execution
              </p>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-3 gap-8 max-w-4xl mx-auto">
              {useCases.map((useCase, index) => (
                <div
                  key={index}
                  className="text-center p-8 rounded-2xl bg-neutral-800/50 border border-neutral-700 hover:border-blue-500/50 transition-all duration-300 group"
                >
                  <div className="w-12 h-12 mx-auto mb-4 rounded-xl bg-gradient-to-br from-blue-500 to-cyan-500 p-3 group-hover:scale-110 transition-transform">
                    <div className="text-white">{useCase.icon}</div>
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

            <div className="max-w-5xl mx-auto">
              <div className="relative group">
                <div className="absolute -inset-4 bg-gradient-to-r from-neutral-500/10 to-neutral-500/10 rounded-3xl blur-xl group-hover:blur-2xl transition-all duration-300"></div>
                <div className="relative aspect-video rounded-2xl overflow-hidden border border-neutral-700">
                  <iframe
                    className="w-full h-full"
                    src="https://www.youtube.com/embed/wY9Q0DhodOQ?si=Lm_anpeBU6Txl5AW"
                    title="Sogen Emulator Overview"
                    frameBorder="0"
                    allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
                    referrerPolicy="strict-origin-when-cross-origin"
                    allowFullScreen
                  ></iframe>
                </div>
              </div>
            </div>
          </div>
        </section>

        {/* CTA Section */}
        <section className="py-24 bg-gradient-to-r from-neutral-900 to-zinc-900">
          <div className="container mx-auto px-6 text-center">
            <h2 className="text-4xl font-bold text-white mb-6">
              Ready to Start Emulating?
            </h2>
            <p className="text-xl text-neutral-300 mb-8 max-w-2xl mx-auto">
              Join researchers worldwide who trust Sogen for their Windows
              emulation needs.
            </p>
            <div className="flex flex-col sm:flex-row gap-4 justify-center">
              <a href="#/playground">
                <Button
                  size="lg"
                  className="bg-white text-neutral-900 hover:bg-neutral-100 px-8 py-6 text-lg font-semibold"
                >
                  <Play className="mr-2 h-5 w-5" />
                  Launch Playground
                </Button>
              </a>
              <a href="https://github.com/momo5502/sogen" target="_blank">
                <Button
                  size="lg"
                  variant="outline"
                  className="border-white text-white hover:bg-white/10 px-8 py-6 text-lg font-semibold"
                >
                  <Download className="mr-2 h-5 w-5" />
                  Download Source
                </Button>
              </a>
            </div>
          </div>
        </section>

        {/* Footer */}
        <footer className="py-16 border-t border-neutral-800">
          <div className="container mx-auto px-6">
            <div className="flex flex-col md:flex-row justify-between items-center">
              <div className="mb-8 md:mb-0 text-center md:text-left">
                <h2 className="text-3xl font-bold bg-gradient-to-r from-blue-400 to-cyan-400 bg-clip-text text-transparent">
                  Sogen
                </h2>
                <p className="mt-2 text-neutral-400 text-lg">
                  Windows User Space Emulator
                </p>
                <p className="mt-1 text-neutral-500 text-sm">
                  Built by{" "}
                  <a
                    href="https://momo5502.com"
                    className="underline"
                    target="_blank"
                  >
                    momo5502
                  </a>{" "}
                  with lots of help of{" "}
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
                  className="text-neutral-400 hover:text-blue-400 transition-colors p-2 rounded-lg hover:bg-neutral-800/50"
                >
                  <Github className="h-6 w-6" />
                </a>
                <a
                  href="#/playground"
                  className="text-neutral-400 hover:text-blue-400 transition-colors p-2 rounded-lg hover:bg-neutral-800/50"
                >
                  <Play className="h-6 w-6" />
                </a>
                <a
                  href="https://github.com/momo5502/sogen/wiki"
                  target="_blank"
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
