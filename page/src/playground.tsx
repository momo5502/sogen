import React from "react";

import { Output } from "@/components/output";

import { Emulator, EmulationState, isFinalState } from "./emulator";
import { Filesystem, setupFilesystem } from "./filesystem";

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
  BarChartSteps,
  CpuFill,
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

interface PlaygroundProps {}
interface PlaygroundState {
  settings: Settings;
  filesystemPromise: Promise<Filesystem> | null;
  filesystem: Filesystem | null;
  emulator: Emulator | null;
  emulationStatus: EmulationStatus | null;
  application: string | undefined;
  drawerOpen: boolean;
  allowWasm64: boolean;
}

function makePercentHandler(
  handler: (percent: number) => void,
): (current: number, total: number) => void {
  const progress = {
    tracked: 0,
  };

  return (current, total) => {
    if (total == 0) {
      return;
    }

    const percent = Math.floor((current * 100) / total);
    const sanePercent = Math.max(Math.min(percent, 100), 0);

    if (sanePercent + 1 > progress.tracked) {
      progress.tracked = sanePercent + 1;
      handler(sanePercent);
    }
  };
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
    this.createEmulator = this.createEmulator.bind(this);
    this.toggleEmulatorState = this.toggleEmulatorState.bind(this);

    this.state = {
      settings: loadSettings(),
      filesystemPromise: null,
      filesystem: null,
      emulator: null,
      emulationStatus: null,
      drawerOpen: false,
      application: undefined,
      allowWasm64: false,
    };
  }

  componentDidMount(): void {
    memory64().then((allowWasm64) => {
      this.setState({ allowWasm64 });
    });
  }

  async resetFilesys() {
    if (!this.state.filesystem) {
      return;
    }

    await this.state.filesystem.delete();

    this.setState({
      filesystemPromise: null,
      filesystem: null,
      drawerOpen: false,
    });

    this.output.current?.clear();

    location.reload();
  }

  _onEmulatorStatusChanged(s: EmulationStatus) {
    this.setState({ emulationStatus: s });
  }

  _onEmulatorStateChanged(s: EmulationState, persistFs: boolean) {
    if (isFinalState(s) && persistFs) {
      this.setState({ filesystemPromise: null, filesystem: null });
      this.initFilesys(true);
    } else {
      this.forceUpdate();
    }
  }

  initFilesys(force: boolean = false) {
    if (!force && this.state.filesystemPromise) {
      return this.state.filesystemPromise;
    }

    const promise = new Promise<Filesystem>((resolve) => {
      if (!force) {
        this.logLine("Loading filesystem...");
      }

      setupFilesystem(
        (current, total, file) => {
          this.logLine(`Processing filesystem (${current}/${total}): ${file}`);
        },
        makePercentHandler((percent) => {
          this.logLine(`Downloading filesystem: ${percent}%`);
        }),
      ).then(resolve);
    });

    promise.then((filesystem) => this.setState({ filesystem }));
    this.setState({ filesystemPromise: promise });

    return promise;
  }

  setDrawerOpen(drawerOpen: boolean) {
    this.setState({ drawerOpen });
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

  async createEmulator(userFile: string) {
    this.state.emulator?.stop();
    this.output.current?.clear();

    this.setDrawerOpen(false);

    this.logLine("Starting emulation...");

    if (this.state.filesystemPromise) {
      await this.state.filesystemPromise;
    }

    const persistFs = this.state.settings.persist;

    const new_emulator = new Emulator(
      (l) => this.logLines(l),
      (s) => this._onEmulatorStateChanged(s, persistFs),
      (s) => this._onEmulatorStatusChanged(s),
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
          description="Playground to test and run Sogen, the Windows user space emulator, right in your browser."
          preload={
            [
              /*"./emulator-worker.js", "./analyzer.js", "./analyzer.wasm"*/
            ]
          }
        />
        <div className="h-[100dvh] flex flex-col">
          <header className="flex shrink-0 items-center gap-2 border-b p-2 overflow-y-auto">
            <a href="#/">
              <Button size="sm" variant="secondary" className="fancy">
                <HouseFill />
              </Button>
            </a>
            <Button size="sm" className="fancy" onClick={this.start}>
              <PlayFill /> <span>Start</span>
            </Button>

            <Button
              disabled={
                !this.state.emulator ||
                isFinalState(this.state.emulator.getState())
              }
              size="sm"
              variant="secondary"
              className="fancy"
              onClick={() => this.state.emulator?.stop()}
            >
              <StopFill /> <span className="hidden sm:inline">Stop</span>
            </Button>
            <Button
              size="sm"
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
                <Button size="sm" variant="secondary" className="fancy">
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
                      runFile={this.createEmulator}
                      resetFilesys={this.resetFilesys}
                      path={["c"]}
                    />
                  </DrawerFooter>
                </DrawerContent>
              </Drawer>
            )}

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
            <div className="items-center absolute z-49 right-0 rounded-bl-lg min-w-[140px] p-2 bg-[var(--background)] pointer-events-none font-medium text-right text-xs whitespace-nowrap leading-6">
              {!this.state.emulationStatus ? (
                <></>
              ) : (
                <>
                  {this.state.emulationStatus.activeThreads}
                  <BarChartSteps className="inline ml-3" />
                  <br />
                  {this.state.emulationStatus.executedInstructions.toLocaleString()}
                  <CpuFill className="inline ml-3" />
                </>
              )}
            </div>
            <div className="flex flex-1 flex-col pl-1 overflow-auto">
              <Output ref={this.output} />
            </div>
          </div>
        </div>
      </>
    );
  }
}
