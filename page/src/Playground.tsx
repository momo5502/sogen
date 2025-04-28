import { useState, useRef, useReducer } from "react";
import { Output } from "@/components/output";

import { AppSidebar } from "@/components/app-sidebar";
import { Separator } from "@/components/ui/separator";
import {
  SidebarInset,
  SidebarProvider,
  SidebarTrigger,
} from "@/components/ui/sidebar";
import { Button } from "@/components/ui/button";

import { Emulator, UserFile, EmulationState } from "./emulator";
import { getFilesystem } from "./filesystem";

import "./App.css";
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";

import { createDefaultSettings } from "./settings";
import { SettingsMenu } from "@/components/settings-menu";

import { PlayFill, StopFill, GearFill, PauseFill } from "react-bootstrap-icons";
import { StatusIndicator } from "@/components/status-indicator";
import { Header } from "./Header";
import {
  ResizableHandle,
  ResizablePanel,
  ResizablePanelGroup,
} from "@/components/ui/resizable";

function selectAndReadFile(): Promise<UserFile> {
  return new Promise((resolve, reject) => {
    const fileInput = document.createElement("input");
    fileInput.type = "file";
    fileInput.accept = ".exe";

    fileInput.addEventListener("change", function (event) {
      const file = (event as any).target.files[0];
      if (file) {
        const reader = new FileReader();

        reader.onload = function (e: ProgressEvent<FileReader>) {
          const arrayBuffer = e.target?.result;
          resolve({
            name: file.name,
            data: arrayBuffer as ArrayBuffer,
          });
        };

        reader.onerror = function (e: ProgressEvent<FileReader>) {
          reject(new Error("Error reading file: " + e.target?.error));
        };

        reader.readAsArrayBuffer(file);
      } else {
        reject(new Error("No file selected"));
      }
    });

    fileInput.click();
  });
}

export function Playground() {
  const output = useRef<Output>(null);
  const [settings, setSettings] = useState(createDefaultSettings());
  const [emulator, setEmulator] = useState<Emulator | null>(null);
  const [, forceUpdate] = useReducer((x) => x + 1, 0);

  function logLine(line: string) {
    output.current?.logLine(line);
  }

  function logLines(lines: string[]) {
    output.current?.logLines(lines);
  }

  function isEmulatorPaused() {
    return emulator && emulator.getState() == EmulationState.Paused;
  }

  function toggleEmulatorState() {
    if (isEmulatorPaused()) {
      emulator?.resume();
    } else {
      emulator?.pause();
    }
  }

  async function createEmulator(userFile: UserFile | null = null) {
    emulator?.stop();
    output.current?.clear();

    logLine("Starting emulation...");

    const fs = await getFilesystem((current, total, file) => {
      logLine(`Processing filesystem (${current}/${total}): ${file}`);
    });

    const new_emulator = new Emulator(fs, logLines, (_) => forceUpdate());
    new_emulator.onTerminate().then(() => setEmulator(null));
    setEmulator(new_emulator);

    new_emulator.start(settings, userFile);
  }

  async function loadAndRunUserFile() {
    const fileBuffer = await selectAndReadFile();
    await createEmulator(fileBuffer);
  }

  return (
    <>
      <Header
        title="Playground - Sogen"
        description="Playground to test and run Sogen, the Windows user space emulator, right in your browser."
      />
      <SidebarProvider defaultOpen={false}>
        <AppSidebar />
        <SidebarInset className="h-[100dvh]">
          <header className="flex shrink-0 items-center gap-2 border-b p-2 overflow-y-auto">
            <SidebarTrigger />
            <Separator orientation="vertical" className="h-4" />
            <Button size="sm" onClick={() => createEmulator()}>
              <PlayFill /> Run Sample
            </Button>
            <Button size="sm" onClick={() => loadAndRunUserFile()}>
              <PlayFill /> Run your .exe
            </Button>
            <Button size="sm" variant="secondary" onClick={() => emulator?.stop()}>
              <StopFill /> Stop Emulation
            </Button>
            <Button size="sm" variant="secondary" onClick={toggleEmulatorState}>
              {isEmulatorPaused() ? (
                <>
                  <PlayFill /> Resume Emulation
                </>
              ) : (
                <>
                  <PauseFill /> Pause Emulation
                </>
              )}
            </Button>

            <Popover>
              <PopoverTrigger asChild>
                <Button size="sm" variant="secondary">
                  <GearFill /> Settings
                </Button>
              </PopoverTrigger>
              <PopoverContent>
                <SettingsMenu settings={settings} onChange={setSettings} />
              </PopoverContent>
            </Popover>
            <div className="text-right flex-1">
              <StatusIndicator
                state={emulator ? emulator.getState() : EmulationState.Stopped}
              />
            </div>
          </header>
          <div className="flex flex-1 flex-col overflow-auto">
            <ResizablePanelGroup direction="horizontal" autoSaveId="debugger-panel-group">
              {/* Left */}
              <ResizablePanel className="resizable-cell">Disassembly</ResizablePanel>
              <ResizableHandle />
              {/* Middle */}
              <ResizablePanel>
                <ResizablePanelGroup direction="vertical" autoSaveId="debugger-panel-middle-group">
                  {/* Middle - Top */}
                  <ResizablePanel>
                    <Output ref={output} />
                  </ResizablePanel>
                  <ResizableHandle />
                  {/* Middle - Bottom */}
                  <ResizablePanel className="resizable-cell">Memory</ResizablePanel>
                </ResizablePanelGroup>
              </ResizablePanel>
              <ResizableHandle />
              {/* Right */}
              <ResizablePanel>
                <ResizablePanelGroup direction="vertical" autoSaveId="debugger-panel-right-group">
                  {/* Right - Top */}
                  <ResizablePanel className="resizable-cell">Registers</ResizablePanel>
                  <ResizableHandle />
                  {/* Right - Bottom */}
                  <ResizablePanel className="resizable-cell">Stack</ResizablePanel>
                </ResizablePanelGroup>
              </ResizablePanel>
            </ResizablePanelGroup>
          </div>
        </SidebarInset>
      </SidebarProvider>
    </>
  );
}
