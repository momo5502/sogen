import React from "react";

import { Output } from "@/components/output";

import { Emulator, EmulationState, isFinalState } from "./emulator";
import {
  Filesystem,
  setupFilesystem,
  windowsToInternalPath,
  setupLinuxFilesystem,
} from "./filesystem";

import { memory64 } from "wasm-feature-detect";

import "./App.css";
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";

import { Settings, loadSettings, saveSettings } from "./settings";
import { SettingsMenu } from "@/components/settings-menu";

import {
  PlayFill,
  StopFill,
  GearFill,
  PauseFill,
  HouseFill,
} from "react-bootstrap-icons";
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
import { FilesystemExplorer } from "./filesystem-explorer";
import { EmulationStatus } from "./emulator";
import { EmulationSummary } from "./components/emulation-summary";
import { downloadBinaryFilePercent } from "./download";

export interface PlaygroundFile {
  file: string;
  storage: string;
}

export interface PlaygroundProps {}

export interface PlaygroundState {
  settings: Settings;
  filesystemPromise?: Promise<Filesystem>;
  filesystem?: Filesystem;
  emulator?: Emulator;
  emulationStatus?: EmulationStatus;
  application?: string;
  drawerOpen: boolean;
  allowWasm64: boolean;
  file?: PlaygroundFile;
}

function decodeFileData(data: string | null): PlaygroundFile | undefined {
  if (!data) {
    return undefined;
  }

  try {
    const jsonData = JSON.parse(atob(data));

    return {
      file: jsonData.file,
      storage: jsonData.storage,
    };
  } catch (e) {
    console.log(e);
  }

  return undefined;
}

interface GlobalThisExt {
  emulateCache?: string | null;
}

function getGlobalThis() {
  return globalThis as GlobalThisExt;
}

export function storeEmulateData(data?: string) {
  getGlobalThis().emulateCache = undefined;

  if (data) {
    localStorage.setItem("emulate", data);
  } else {
    localStorage.removeItem("emulate");
  }
}

function getEmulateData() {
  const gt = getGlobalThis();
  if (gt.emulateCache) {
    return gt.emulateCache;
  }

  const emulateData = localStorage.getItem("emulate");
  localStorage.removeItem("emulate");

  gt.emulateCache = emulateData;
  return emulateData;
}

export class Playground extends React.Component<
  PlaygroundProps,
  PlaygroundState
> {
  private output: React.RefObject<Output | null>;
  private iconCache: Map<string, string | null> = new Map();

  constructor(props: PlaygroundProps) {
    super(props);

    this.output = React.createRef();

    this.start = this.start.bind(this);
    this.resetFilesys = this.resetFilesys.bind(this);
    this.startEmulator = this.startEmulator.bind(this);
    this.fetchExecutionTime = this.fetchExecutionTime.bind(this);
    this.toggleEmulatorState = this.toggleEmulatorState.bind(this);

    this.state = {
      settings: loadSettings(),
      drawerOpen: false,
      allowWasm64: false,
      file: decodeFileData(getEmulateData()),
    };
  }

  componentDidMount(): void {
    memory64().then((allowWasm64) => {
      this.setState({ allowWasm64 });
    });

    if (this.state.file) {
      this.emulateRemoteFile(this.state.file);
    }
  }

  componentWillUnmount(): void {
    this.state.emulator?.stop();
  }

  resetFilesystemState() {
    this.setState({
      filesystemPromise: undefined,
      filesystem: undefined,
      drawerOpen: false,
    });
  }

  fetchExecutionTime() {
    return this.state.emulator ? this.state.emulator.getExecutionTime() : 0;
  }

  async resetFilesys() {
    if (!this.state.filesystem) {
      return;
    }

    await this.state.filesystem.delete();

    this.resetFilesystemState();
    this.output.current?.clear();
    location.reload();
  }

  _onEmulatorStatusChanged(s: EmulationStatus) {
    this.setState({ emulationStatus: s });
  }

  _onEmulatorStateChanged(s: EmulationState, persistFs: boolean) {
    if (isFinalState(s) && persistFs) {
      this.setState({ filesystemPromise: undefined, filesystem: undefined });
      this.initFilesys(true);
    } else {
      this.forceUpdate();
    }
  }

  initFilesys(force: boolean = false) {
    if (!force && this.state.filesystemPromise) {
      return this.state.filesystemPromise;
    }

    const isLinux = this.state.settings.mode === "linux";

    const promise = new Promise<Filesystem>((resolve, reject) => {
      if (!force) {
        this.output.current?.clear();
        this.logLine("Loading filesystem...");
      }

      const setup = isLinux
        ? setupLinuxFilesystem()
        : setupFilesystem(
            (current, total, file) => {
              this.logLine(
                `Processing filesystem (${current}/${total}): ${file}`,
              );
            },
            (percent) => {
              this.logLine(`Downloading filesystem: ${percent}%`);
            },
          );

      setup.then(resolve).catch(reject);
    });

    promise.then((filesystem) => this.setState({ filesystem }));
    this.setState({ filesystemPromise: promise });

    promise.catch((e) => {
      console.log(e);
      this.logLine("Failed to fetch filesystem:");
      this.logLine(e.toString());
      this.resetFilesystemState();
    });

    return promise;
  }

  setDrawerOpen(drawerOpen: boolean) {
    this.setState({ drawerOpen });
  }

  async downloadFileToFilesystem(file: PlaygroundFile) {
    const fs = await this.initFilesys();

    const fileData = await downloadBinaryFilePercent(
      file.storage,
      (percent) => {
        this.logLine(`Downloading binary: ${percent}%`);
      },
    );

    const isLinux = this.state.settings.mode === "linux";
    const internalPath = isLinux
      ? `/root/${file.file}`
      : windowsToInternalPath(file.file);

    await fs.storeFiles([
      {
        name: internalPath,
        data: fileData,
      },
    ]);
  }

  async emulateRemoteFile(file: PlaygroundFile) {
    await this.downloadFileToFilesystem(file);
    await this.startEmulator(file.file);
  }

  async start() {
    await this.initFilesys();
    this.setDrawerOpen(true);
  }

  logLine(line: string) {
    this.output.current?.logLine(line);
  }

  logLines(lines: string[]) {
    this.output.current?.logLines(lines);
  }

  isEmulatorPaused() {
    return (
      this.state.emulator &&
      this.state.emulator.getState() == EmulationState.Paused
    );
  }

  toggleEmulatorState() {
    if (this.isEmulatorPaused()) {
      this.state.emulator?.resume();
    } else {
      this.state.emulator?.pause();
    }
  }

  async startEmulator(userFile: string) {
    this.state.emulator?.stop();
    this.output.current?.clear();

    this.setDrawerOpen(false);

    if (this.state.filesystemPromise) {
      await this.state.filesystemPromise;
    }

    const persistFs = this.state.settings.persist;

    const mode = this.state.settings.mode;
    const new_emulator = new Emulator(
      (l) => this.logLines(l),
      (s) => this._onEmulatorStateChanged(s, persistFs),
      (s) => this._onEmulatorStatusChanged(s),
      mode,
    );
    //new_emulator.onTerminate().then(() => this.setState({ emulator: null }));

    this.setState({ emulator: new_emulator, application: userFile });

    new_emulator.start(this.state.settings, userFile);
  }

  render() {
    return (
      <>
        <Header
          title="Sogen - Playground"
          description="Playground to test and run Sogen, a Windows and Linux user space emulator, right in your browser."
          preload={
            [
              /*"./emulator-worker.js", "./analyzer.js", "./analyzer.wasm"*/
            ]
          }
        />
        <div className="h-[100dvh] flex flex-col">
          <header className="flex shrink-0 items-center gap-2 border-b p-2 overflow-y-auto">
            <a title="Home" href="#/">
              <Button
                size="sm"
                variant="secondary"
                className="fancy"
                title="Home Button"
              >
                <HouseFill />
              </Button>
            </a>
            <Button
              size="sm"
              className="fancy"
              onClick={this.start}
              title="Start"
            >
              <PlayFill /> <span>Start</span>
            </Button>

            <Button
              disabled={
                !this.state.emulator ||
                isFinalState(this.state.emulator.getState())
              }
              size="sm"
              title="Stop"
              variant="secondary"
              className="fancy"
              onClick={() => this.state.emulator?.stop()}
            >
              <StopFill /> <span className="hidden sm:inline">Stop</span>
            </Button>
            <Button
              size="sm"
              title={this.isEmulatorPaused() ? "Resume" : "Pause"}
              disabled={
                !this.state.emulator ||
                isFinalState(this.state.emulator.getState())
              }
              variant="secondary"
              className="fancy"
              onClick={this.toggleEmulatorState}
            >
              {this.isEmulatorPaused() ? (
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
                <Button
                  size="sm"
                  variant="secondary"
                  className="fancy"
                  title="Settings"
                >
                  <GearFill />{" "}
                  <span className="hidden sm:inline">Settings</span>
                </Button>
              </PopoverTrigger>
              <PopoverContent>
                <SettingsMenu
                  settings={this.state.settings}
                  allowWasm64={this.state.allowWasm64}
                  onChange={(s) => {
                    saveSettings(s);
                    this.setState({ settings: s });
                  }}
                />
              </PopoverContent>
            </Popover>

            {!this.state.filesystem ? (
              <></>
            ) : (
              <Drawer
                open={this.state.drawerOpen}
                onOpenChange={(o) => this.setState({ drawerOpen: o })}
              >
                <DrawerContent className="!will-change-auto">
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
                      filesystem={this.state.filesystem}
                      iconCache={this.iconCache}
                      runFile={this.startEmulator}
                      resetFilesys={this.resetFilesys}
                      path={this.state.settings.mode === "linux" ? [] : ["c"]}
                      linuxMode={this.state.settings.mode === "linux"}
                    />
                  </DrawerFooter>
                </DrawerContent>
              </Drawer>
            )}

            {/* Separator */}
            <div className="flex-1"></div>

            <div className="text-right items-center">
              <StatusIndicator
                application={this.state.application}
                state={
                  this.state.emulator
                    ? this.state.emulator.getState()
                    : EmulationState.Stopped
                }
              />
            </div>
          </header>
          <div className="flex flex-1">
            <EmulationSummary
              status={this.state.emulationStatus}
              executionTimeFetcher={this.fetchExecutionTime}
            />
            <div className="flex flex-1 flex-col pl-1 overflow-auto">
              <Output ref={this.output} />
            </div>
          </div>
        </div>
      </>
    );
  }
}
