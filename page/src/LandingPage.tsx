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
} from "lucide-react";
import { Header } from "./Header";

export function LandingPage() {
  const features = [
    {
      icon: <Cpu className="h-6 w-6 text-[var(--primary)]" />,
      title: "Syscall-Level Emulation",
      description:
        "Operates at syscall level, leveraging existing system DLLs instead of reimplementing Windows APIs",
    },
    {
      icon: <Database className="h-6 w-6 text-[var(--primary)]" />,
      title: "Advanced Memory Management",
      description:
        "Supports Windows-specific memory types including reserved, committed, built on top of Unicorn's memory management",
    },
    {
      icon: <FileCode className="h-6 w-6 text-[var(--primary)]" />,
      title: "Complete PE Loading",
      description:
        "Handles executable and DLL loading with proper memory mapping, relocations, and TLS",
    },
    {
      icon: <Shield className="h-6 w-6 text-[var(--primary)]" />,
      title: "Exception Handling",
      description:
        "Implements Windows structured exception handling (SEH) with proper exception dispatcher and unwinding support",
    },
    {
      icon: <Layers className="h-6 w-6 text-[var(--primary)]" />,
      title: "Threading Support",
      description: "Provides a scheduled (round-robin) threading model",
    },
    {
      icon: <Terminal className="h-6 w-6 text-[var(--primary)]" />,
      title: "Debugging Interface",
      description:
        "Implements GDB serial protocol for integration with common debugging tools",
    },
  ];

  return (
    <>
      <Header
        title="Sogen - Windows User Space Emulator"
        description="Sogen is a high-performance Windows user space emulator operating at syscall level that can emulate windows processes. It is ideal for security-, DRM- or malware research."
      />
      <div className="flex flex-col min-h-screen">
        {/* Hero Section */}
        <header className="bg-gradient-to-r from-blue-600 to-cyan-500 py-16 md:py-24">
          <div className="container mx-auto px-4 md:px-6">
            <div className="flex flex-col md:flex-row items-center justify-between">
              <div className="w-full md:w-1/2 space-y-6 text-center md:text-left text-white">
                <h1 className="text-4xl md:text-6xl font-bold tracking-tight">
                  Sogen
                </h1>
                <p className="text-xl md:text-2xl font-light">
                  High-performance Windows user space emulator operating at
                  syscall level
                </p>
                <div className="flex flex-wrap gap-4 justify-center md:justify-start">
                  <a href="#/playground" target="_blank">
                    <Button
                      size="lg"
                      className="bg-white text-blue-700 hover:bg-blue-50"
                    >
                      <Play className="mr-2 h-5 w-5" />
                      Try Online
                    </Button>
                  </a>
                  <a href="https://github.com/momo5502/sogen" target="_blank">
                    <Button
                      size="lg"
                      variant="outline"
                      className="border-white text-white hover:bg-white/10"
                    >
                      <Github className="mr-2 h-5 w-5" />
                      GitHub
                    </Button>
                  </a>
                </div>
              </div>
              {/*<div className="w-full md:w-1/2 mt-8 md:mt-0 flex justify-center md:justify-end">
              <div className="relative w-full max-w-md">
                <div className="absolute inset-0 bg-gradient-to-r from-blue-500/20 to-indigo-500/20 rounded-lg blur-xl"></div>
                <img
                  src="/api/placeholder/600/400"
                  alt="Sogen Emulator"
                  className="relative rounded-lg shadow-xl w-full"
                />
              </div>
            </div>*/}
            </div>
          </div>
        </header>

        {/* Key Features */}
        <section className="py-16 md:py-24">
          <div className="container mx-auto px-4 md:px-6">
            <div className="text-center mb-12">
              <h2 className="text-3xl md:text-4xl font-bold mb-4">
                Key Features
              </h2>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-6">
              {features.map((feature, index) => (
                <Card key={index} className="hover:shadow-lg transition-shadow">
                  <CardHeader>
                    <div className="mb-2">{feature.icon}</div>
                    <CardTitle>{feature.title}</CardTitle>
                  </CardHeader>
                  <CardContent>
                    <p className="text-gray-600 dark:text-gray-400">
                      {feature.description}
                    </p>
                  </CardContent>
                </Card>
              ))}
            </div>
          </div>
        </section>

        {/* Video Section */}
        <section className="bg-zinc-900 py-16 md:py-24">
          <div className="container mx-auto px-4 md:px-6">
            <div className="text-center mb-12">
              <h2 className="text-3xl md:text-4xl font-bold mb-4">
                See Sogen in Action
              </h2>
              <p className="text-lg text-gray-600 dark:text-gray-400 max-w-3xl mx-auto">
                Watch an overview of the emulator's capabilities and see how it
                can help with your research.
              </p>
            </div>

            <div className="max-w-4xl mx-auto">
              <div className="relative aspect-video rounded-2xl shadow-2xl">
                <iframe
                  className="rounded-2xl"
                  width="100%"
                  height="100%"
                  src="https://www.youtube.com/embed/wY9Q0DhodOQ?si=Lm_anpeBU6Txl5AW"
                  title="YouTube video player"
                  frameBorder="0"
                  allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
                  referrerPolicy="strict-origin-when-cross-origin"
                  allowFullScreen
                ></iframe>
              </div>
              <div className="mt-4 text-center"></div>
            </div>
          </div>
        </section>

        {/* Footer */}
        <footer className="py-12">
          <div className="container mx-auto px-4 md:px-6">
            <div className="flex flex-col md:flex-row justify-between items-center">
              <div className="mb-6 md:mb-0">
                <h2 className="text-2xl font-bold">Sogen</h2>
                <p className="mt-2 text-gray-400">
                  Windows User Space Emulator
                </p>
              </div>
              <div className="flex space-x-6">
                <a
                  href="https://github.com/momo5502/sogen"
                  target="_blank"
                  className="hover:text-blue-400"
                >
                  <Github className="h-6 w-6" />
                </a>
                <a
                  href="#/playground"
                  target="_blank"
                  className="hover:text-blue-400"
                >
                  <ExternalLink className="h-6 w-6" />
                </a>
              </div>
            </div>
          </div>
        </footer>
      </div>
    </>
  );
}
