import { useState, useRef, useReducer } from "react";
import { Output } from "@/components/output";

import { Separator } from "@/components/ui/separator";

import { Emulator, EmulationState } from "./emulator";
import { Filesystem, setupFilesystem } from "./filesystem";

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

import { Button } from "@/components/ui/button";

import {
  Drawer,
  DrawerContent,
  DrawerDescription,
  DrawerFooter,
  DrawerHeader,
  DrawerTitle,
} from "@/components/ui/drawer";
import { FilesystemExplorer } from "./FilesystemExplorer";

export function Playground() {
  const output = useRef<Output>(null);
  const [settings, setSettings] = useState(createDefaultSettings());
  const [emulator, setEmulator] = useState<Emulator | null>(null);
  const [drawerOpen, setDrawerOpen] = useState<boolean>(false);
  const [filesystem, setFilesystem] = useState<Filesystem | null>(null);
  const [filesystemPromise, setFilesystemPromise] =
    useState<Promise<Filesystem> | null>(null);
  const [, forceUpdate] = useReducer((x) => x + 1, 0);

  async function resetFilesys() {
    const fs = await initFilesys();
    await fs.delete();

    setFilesystemPromise(null);
    setFilesystem(null);
    setDrawerOpen(false);

    output.current?.clear();
  }

  function initFilesys() {
    if (filesystemPromise) {
      return filesystemPromise;
    }

    const promise = new Promise<Filesystem>((resolve) => {
      logLine("Loading filesystem...");
      setupFilesystem((current, total, file) => {
        logLine(`Processing filesystem (${current}/${total}): ${file}`);
      }).then(resolve);
    });

    promise.then(setFilesystem);
    setFilesystemPromise(promise);

    return promise;
  }

  async function start() {
    await initFilesys();
    setDrawerOpen(true);
  }

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

  async function createEmulator(userFile: string) {
    emulator?.stop();
    output.current?.clear();

    setDrawerOpen(false);

    logLine("Starting emulation...");

    if (filesystemPromise) {
      await filesystemPromise;
    }

    const new_emulator = new Emulator(logLines, (_) => forceUpdate());
    new_emulator.onTerminate().then(() => setEmulator(null));
    setEmulator(new_emulator);

    new_emulator.start(settings, userFile);
  }

  return (
    <>
      <Header
        title="Playground - Sogen"
        description="Playground to test and run Sogen, the Windows user space emulator, right in your browser."
      />
      <div className="h-[100dvh] flex flex-col">
        <header className="flex shrink-0 items-center gap-2 border-b p-2 overflow-y-auto">
          <Button size="sm" className="fancy" onClick={start}>
            <PlayFill /> <span>Start</span>
          </Button>

          <Button
            disabled={!emulator}
            size="sm"
            variant="secondary"
            className="fancy"
            onClick={() => emulator?.stop()}
          >
            <StopFill /> <span className="hidden sm:inline">Stop</span>
          </Button>
          <Button
            size="sm"
            disabled={!emulator}
            variant="secondary"
            className="fancy"
            onClick={toggleEmulatorState}
          >
            {isEmulatorPaused() ? (
              <>
                <PlayFill /> <span className="hidden sm:inline">Resume</span>
              </>
            ) : (
              <>
                <PauseFill /> <span className="hidden sm:inline">Pause</span>
              </>
            )}
          </Button>

          <Popover>
            <PopoverTrigger asChild>
              <Button size="sm" variant="secondary" className="fancy">
                <GearFill /> <span className="hidden sm:inline">Settings</span>
              </Button>
            </PopoverTrigger>
            <PopoverContent>
              <SettingsMenu settings={settings} onChange={setSettings} />
            </PopoverContent>
          </Popover>

          {!filesystem ? (
            <></>
          ) : (
            <Drawer open={drawerOpen} onOpenChange={setDrawerOpen}>
              <DrawerContent>
                <DrawerHeader>
                  <DrawerTitle className="hidden">
                    Filesystem Explorer
                  </DrawerTitle>
                  <DrawerDescription className="hidden">
                    Filesystem Explorer
                  </DrawerDescription>
                </DrawerHeader>
                <DrawerFooter>
                  <FilesystemExplorer
                    filesystem={filesystem}
                    runFile={createEmulator}
                    resetFilesys={resetFilesys}
                    path={["c"]}
                  />
                </DrawerFooter>
              </DrawerContent>
            </Drawer>
          )}

          <div className="text-right flex-1">
            <StatusIndicator
              state={emulator ? emulator.getState() : EmulationState.Stopped}
            />
          </div>
        </header>
        <div className="flex flex-1 flex-col gap-2 p-2 overflow-auto">
          <Output ref={output} />
        </div>
      </div>
    </>
  );
}
