import React from "react";

import { Output } from "@/components/output";

import { Emulator, EmulationState } from "./emulator";
import { Filesystem, setupFilesystem } from "./filesystem";

import "./App.css";
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from "@/components/ui/popover";

import { createDefaultSettings, Settings } from "./settings";
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
import { FilesystemExplorer } from "./filesystem-explorer";

interface PlaygroundProps {}
interface PlaygroundState {
  settings: Settings;
  filesystemPromise: Promise<Filesystem> | null;
  filesystem: Filesystem | null;
  emulator: Emulator | null;
  drawerOpen: boolean;
}

export class Playground extends React.Component<
  PlaygroundProps,
  PlaygroundState
> {
  private output: React.RefObject<Output | null>;

  constructor(props: PlaygroundProps) {
    super(props);

    this.output = React.createRef();

    this.start = this.start.bind(this);
    this.resetFilesys = this.resetFilesys.bind(this);
    this.createEmulator = this.createEmulator.bind(this);
    this.toggleEmulatorState = this.toggleEmulatorState.bind(this);

    this.state = {
      settings: createDefaultSettings(),
      filesystemPromise: null,
      filesystem: null,
      emulator: null,
      drawerOpen: false,
    };
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
  }

  initFilesys() {
    if (this.state.filesystemPromise) {
      return this.state.filesystemPromise;
    }

    const promise = new Promise<Filesystem>((resolve) => {
      this.logLine("Loading filesystem...");
      setupFilesystem((current, total, file) => {
        this.logLine(`Processing filesystem (${current}/${total}): ${file}`);
      }).then(resolve);
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

    const new_emulator = new Emulator(
      (l) => this.logLines(l),
      (_) => this.forceUpdate(),
    );
    new_emulator.onTerminate().then(() => this.setState({ emulator: null }));

    this.setState({ emulator: new_emulator });

    new_emulator.start(this.state.settings, userFile);
  }

  render() {
    return (
      <>
        <Header
          title="Playground - Sogen"
          description="Playground to test and run Sogen, the Windows user space emulator, right in your browser."
        />
        <div className="h-[100dvh] flex flex-col">
          <header className="flex shrink-0 items-center gap-2 border-b p-2 overflow-y-auto">
            <Button size="sm" className="fancy" onClick={this.start}>
              <PlayFill /> <span>Start</span>
            </Button>

            <Button
              disabled={!this.state.emulator}
              size="sm"
              variant="secondary"
              className="fancy"
              onClick={() => this.state.emulator?.stop()}
            >
              <StopFill /> <span className="hidden sm:inline">Stop</span>
            </Button>
            <Button
              size="sm"
              disabled={!this.state.emulator}
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
                  onChange={(s) => this.setState({ settings: s })}
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
                      filesystem={this.state.filesystem}
                      runFile={this.createEmulator}
                      resetFilesys={this.resetFilesys}
                      path={["c"]}
                    />
                  </DrawerFooter>
                </DrawerContent>
              </Drawer>
            )}

            <div className="text-right flex-1">
              <StatusIndicator
                state={
                  this.state.emulator
                    ? this.state.emulator.getState()
                    : EmulationState.Stopped
                }
              />
            </div>
          </header>
          <div className="flex flex-1 flex-col gap-2 p-2 overflow-auto">
            <Output ref={this.output} />
          </div>
        </div>
      </>
    );
  }
}
