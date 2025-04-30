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

import {
  PlayFill,
  StopFill,
  GearFill,
  PauseFill,
  FileEarmarkCheckFill,
  ImageFill,
} from "react-bootstrap-icons";
import { StatusIndicator } from "@/components/status-indicator";
import { Header } from "./Header";

import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
  DropdownMenuGroup,
} from "@/components/ui/dropdown-menu";
import { Button } from "@/components/ui/button";

import {
  Drawer,
  DrawerClose,
  DrawerContent,
  DrawerDescription,
  DrawerFooter,
  DrawerHeader,
  DrawerTitle,
  DrawerTrigger,
} from "@/components/ui/drawer";
import { FilesystemExplorer } from "./FilesystemExplorer";

/*function selectAndReadFile(): Promise<UserFile> {
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
}*/

export function Playground() {
  const output = useRef<Output>(null);
  const [settings, setSettings] = useState(createDefaultSettings());
  const [emulator, setEmulator] = useState<Emulator | null>(null);
  const [filesystem, setFilesystem] = useState<Filesystem | null>(null);
  const [filesystemPromise, setFilesystemPromise] =
    useState<Promise<Filesystem> | null>(null);
  const [, forceUpdate] = useReducer((x) => x + 1, 0);

  if (!filesystemPromise) {
    const promise = new Promise<Filesystem>((resolve) => {
      setupFilesystem((current, total, file) => {
        logLine(`Processing filesystem (${current}/${total}): ${file}`);
      }).then(resolve);
    });

    promise.then(setFilesystem);

    setFilesystemPromise(promise);
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

    logLine("Starting emulation...");

    if (filesystemPromise) {
      await filesystemPromise;
    }

    const new_emulator = new Emulator(logLines, (_) => forceUpdate());
    new_emulator.onTerminate().then(() => setEmulator(null));
    setEmulator(new_emulator);

    new_emulator.start(settings, userFile);
  }

  async function loadAndRunUserFile() {
    //const fileBuffer = await selectAndReadFile();
    //await createEmulator(fileBuffer);
  }

  return (
    <>
      <Header
        title="Playground - Sogen"
        description="Playground to test and run Sogen, the Windows user space emulator, right in your browser."
      />
      <div className="h-[100dvh] flex flex-col">
        <header className="flex shrink-0 items-center gap-2 border-b p-2 overflow-y-auto">
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button size="sm" className="fancy">
                <PlayFill /> Run
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent className="w-56">
              <DropdownMenuLabel>Run Application</DropdownMenuLabel>
              <DropdownMenuSeparator />
              <DropdownMenuGroup>
                <DropdownMenuItem
                  onClick={() => createEmulator("c:/test-sample.exe")}
                >
                  <ImageFill className="mr-2" />
                  <span>Select Sample</span>
                </DropdownMenuItem>
                <DropdownMenuItem onClick={() => loadAndRunUserFile()}>
                  <FileEarmarkCheckFill className="mr-2" />
                  <span>Select your .exe</span>
                </DropdownMenuItem>
              </DropdownMenuGroup>
            </DropdownMenuContent>
          </DropdownMenu>

          <Button
            disabled={!emulator}
            size="sm"
            variant="secondary"
            className="fancy"
            onClick={() => emulator?.stop()}
          >
            <StopFill /> Stop
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
                <PlayFill /> Resume
              </>
            ) : (
              <>
                <PauseFill /> Pause
              </>
            )}
          </Button>

          <Popover>
            <PopoverTrigger asChild>
              <Button size="sm" variant="secondary" className="fancy">
                <GearFill /> Settings
              </Button>
            </PopoverTrigger>
            <PopoverContent>
              <SettingsMenu settings={settings} onChange={setSettings} />
            </PopoverContent>
          </Popover>

          {!filesystem ? (
            <></>
          ) : (
            <Drawer>
              <DrawerTrigger asChild>
                <Button size="sm" variant="secondary" className="fancy">
                  <GearFill /> Filesystem
                </Button>
              </DrawerTrigger>
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
