import React from "react";
import { attachSogenUiHost } from "./web-ui-host";
import { MacosGuestScreen } from "./macos/guest-screen";
import { MacosTraceView, MacosTraceViewHandle } from "./macos/trace-view";
import {
  MacosAttach,
  MacosAttachment,
  SERVED_ROOT,
} from "./macos/bundle-attach";
import { discoverRootFiles } from "./macos/root-cache";
import { MacosFilesystemExplorer } from "./macos/filesystem-view";

import { Output } from "@/components/output";

import { Emulator, EmulationState, isFinalState } from "./emulator";
import {
  Filesystem,
  setupFilesystem,
  windowsToInternalPath,
  setupLinuxFilesystem,
  setupMacosFilesystem,
  runtimeRoots,
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
  FileEarmarkArrowDownFill,
  Memory,
  BugFill,
} from "react-bootstrap-icons";
import { MemoryView } from "./components/memory-view";
import { DebuggerView } from "./components/debugger-view";
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

// macOS-only knobs on a run. attachRoot defaults to the mode, not to false: every macOS binary that is
// not statically linked needs the shared cache, and only the demo buttons have a reason to say
// otherwise. fromServedRoot is what a run launched out of the filesystem view sets -- the program itself
// lives on the served root, so the worker has to resolve paths against it even though the page attached
// nothing by hand.
export interface StartOptions {
  attachRoot?: boolean;
  fromServedRoot?: boolean;
}

// GUEST_ROOT in page/public/macos-emulator-worker.js: everything attached is addressed by its host path,
// because the emulator resolves a guest path through the emulation root before it asks for bytes.
const MACOS_GUEST_ROOT = "/root";

export interface PlaygroundProps {}

export interface PlaygroundState {
  settings: Settings;
  filesystemPromise?: Promise<Filesystem>;
  filesystem?: Filesystem;
  emulator?: Emulator;
  emulationStatus?: EmulationStatus;
  application?: string;
  drawerOpen: boolean;
  memoryViewOpen: boolean;
  debuggerOpen: boolean;
  allowWasm64: boolean;
  uiWindowCount: number;
  uiPanelWidth: number;
  file?: PlaygroundFile;
  macosAttachment?: MacosAttachment;
}

function downloadTextFile(content: string, filename = "output.txt") {
  const blob = new Blob([content], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
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
  private macosTrace: React.RefObject<MacosTraceViewHandle | null>;
  private uiCanvas: React.RefObject<HTMLCanvasElement | null>;
  private uiHostDispose?: () => void;
  private uiPanelDragging = false;
  private iconCache: Map<string, string | null> = new Map();

  constructor(props: PlaygroundProps) {
    super(props);

    this.output = React.createRef();
    this.macosTrace = React.createRef();
    this.uiCanvas = React.createRef();

    this.start = this.start.bind(this);
    this.resetFilesys = this.resetFilesys.bind(this);
    this.startEmulator = this.startEmulator.bind(this);
    this.fetchExecutionTime = this.fetchExecutionTime.bind(this);
    this.toggleEmulatorState = this.toggleEmulatorState.bind(this);

    this.state = {
      settings: loadSettings(),
      drawerOpen: false,
      memoryViewOpen: false,
      debuggerOpen: false,
      allowWasm64: false,
      uiWindowCount: 0,
      uiPanelWidth: Math.min(720, Math.round(window.innerWidth * 0.45)),
      file: decodeFileData(getEmulateData()),
    };
  }

  componentDidMount(): void {
    memory64().then((allowWasm64) => {
      this.setState({ allowWasm64 });
    });

    window.addEventListener("mousemove", this.onUiPanelResizeMove);
    window.addEventListener("mouseup", this.onUiPanelResizeEnd);

    if (this.state.file) {
      this.emulateRemoteFile(this.state.file);
    }
  }

  componentWillUnmount(): void {
    window.removeEventListener("mousemove", this.onUiPanelResizeMove);
    window.removeEventListener("mouseup", this.onUiPanelResizeEnd);
    document.body.style.userSelect = "";
    this.uiPanelDragging = false;
    this.uiHostDispose?.();
    this.state.emulator?.stop();
  }

  onUiPanelResizeMove = (event: MouseEvent) => {
    if (!this.uiPanelDragging) {
      return;
    }

    const next = window.innerWidth - event.clientX;
    this.setState({
      uiPanelWidth: Math.min(Math.max(next, 320), window.innerWidth - 160),
    });
  };

  onUiPanelResizeEnd = () => {
    if (!this.uiPanelDragging) {
      return;
    }

    this.uiPanelDragging = false;
    document.body.style.userSelect = "";
  };

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

    const mode = this.state.settings.mode;

    const promise = new Promise<Filesystem>((resolve, reject) => {
      if (!force) {
        this.output.current?.clear();
        this.logLine("Loading filesystem...");
      }

      const setup =
        mode === "linux"
          ? setupLinuxFilesystem()
          : mode === "macos"
            ? setupMacosFilesystem()
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

    const mode = this.state.settings.mode;
    const internalPath =
      mode === "linux"
        ? `${runtimeRoots.linux}/${file.file}`
        : mode === "macos"
          ? `${runtimeRoots.macos}/${file.file}`
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
    // macOS's guest filesystem is the served root plus whatever the attach panel holds, neither of which
    // is the IDBFS tree initFilesys() builds. MacosFilesystemExplorer reads those two directly, so
    // nothing here may touch the module's filesystem: listing a root that was never created is what
    // threw ErrnoError out of render and unmounted the whole app.
    if (this.state.settings.mode === "macos") {
      this.setDrawerOpen(true);
      return;
    }

    await this.initFilesys();
    this.setDrawerOpen(true);
  }

  logLine(line: string) {
    this.output.current?.logLine(line);
  }

  logLines(lines: string[]) {
    this.output.current?.logLines(lines);
  }

  // macOS mode mounts the trace instead of the terminal, so the export follows whichever is there.
  exportLog() {
    if (this.macosTrace.current) {
      downloadTextFile(this.macosTrace.current.getLines().join("\n"));
      return;
    }

    if (!this.output.current) {
      return;
    }

    const log = this.output.current.getLines();
    downloadTextFile(log.map((l) => l.text).join("\n"));
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

  async startEmulator(userFile: string, options: StartOptions = {}) {
    this.state.emulator?.stop();
    this.output.current?.clear();

    this.setDrawerOpen(false);
    this.setState({ memoryViewOpen: false });

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

    this.uiHostDispose?.();
    this.uiHostDispose = undefined;
    if (mode !== "macos" && this.uiCanvas.current) {
      const host = attachSogenUiHost(
        new_emulator.worker,
        this.uiCanvas.current,
        {
          onWindowCountChanged: (uiWindowCount) =>
            this.setState({ uiWindowCount }),
        },
      );
      this.uiHostDispose = host.dispose;
    }

    // The shared cache a dynamically linked macOS binary needs is served, not bundled with the run
    // message page/src/emulator.ts sends -- there is nowhere else in that message for it. Posted to the
    // worker directly, before start() posts "run", the same way src/macos-web/app.js attaches it before
    // pressing Run.
    const attachRoot = options.attachRoot ?? mode === "macos";
    const attachment = this.state.macosAttachment;

    // Only something the user actually attached turns the served root into the answer for every path the
    // page did not attach by hand. A real .app needs that -- AppKit reads .car catalogues out of
    // /System/Library/CoreServices before its first window -- but it is not free: every filesystem miss
    // becomes a synchronous directory listing, measured at 6.85 s against 5.28 s on the paint demo. No
    // demo can need it, so no demo pays for it. Tested on content rather than on the attachment
    // existing, because Clear leaves an empty attachment behind rather than dropping it.
    const useServedFallback =
      (attachment?.fileCount ?? 0) > 0 || !!options.fromServedRoot;

    if (mode === "macos" && (attachRoot || useServedFallback)) {
      const hosted: { path: string; url: string }[] = [];

      if (attachRoot) {
        try {
          const rootFiles = await discoverRootFiles();
          hosted.push(
            ...rootFiles.map((file) => ({
              path: `${MACOS_GUEST_ROOT}${file.guestPath}`,
              url: file.url,
            })),
          );
        } catch (e) {
          this.logLine(`Could not attach the macOS root: ${e}`);
        }
      }

      if (useServedFallback && attachment) {
        hosted.push(
          ...attachment.hosted.map((file) => ({
            path: `${MACOS_GUEST_ROOT}${file.path}`,
            url: file.url,
          })),
        );

        // Handles, not contents: the worker's range bridge reads slices of these with FileReaderSync,
        // which is what keeps attaching a bundle -- or a root -- instant rather than a silent copy.
        if (attachment.files.length) {
          new_emulator.worker.postMessage({
            t: "files",
            files: attachment.files.map(([path, file]) => [
              `${MACOS_GUEST_ROOT}${path}`,
              file,
            ]),
          });
        }
      }

      new_emulator.worker.postMessage({
        t: "attach-root",
        hosted,
        dirs: useServedFallback
          ? (attachment?.dirs.map((dir) => ({
              path: `${MACOS_GUEST_ROOT}${dir.path}`,
              url: dir.url,
            })) ?? [])
          : [],
        served: useServedFallback ? `${location.origin}${SERVED_ROOT}` : null,
      });
    }

    this.setState({
      emulator: new_emulator,
      application: userFile,
      uiWindowCount: 0,
    });

    new_emulator.start(this.state.settings, userFile, this.state.debuggerOpen);
  }

  _renderFilesystemExplorer() {
    if (this.state.settings.mode === "macos") {
      return (
        <MacosFilesystemExplorer
          attachment={this.state.macosAttachment}
          runFile={(guestPath) =>
            this.startEmulator(guestPath, { fromServedRoot: true })
          }
        />
      );
    }

    const filesystem = this.state.filesystem;
    if (!filesystem) {
      return null;
    }

    return (
      <FilesystemExplorer
        filesystem={filesystem}
        iconCache={this.iconCache}
        runFile={this.startEmulator}
        resetFilesys={this.resetFilesys}
        path={this.state.settings.mode === "windows" ? ["c"] : []}
        linuxMode={this.state.settings.mode === "linux"}
      />
    );
  }

  _renderFilesystemDrawer() {
    const explorer = this._renderFilesystemExplorer();
    if (!explorer) {
      return <></>;
    }

    return (
      <Drawer
        open={this.state.drawerOpen}
        onOpenChange={(o) => this.setState({ drawerOpen: o })}
      >
        <DrawerContent className="will-change-auto!">
          <DrawerHeader>
            <DrawerTitle className="hidden">Filesystem Explorer</DrawerTitle>
            <DrawerDescription className="hidden">
              Filesystem Explorer
            </DrawerDescription>
          </DrawerHeader>
          <DrawerFooter>{explorer}</DrawerFooter>
        </DrawerContent>
      </Drawer>
    );
  }

  render() {
    return (
      <>
        <Header
          title="Sogen - Playground"
          description="Playground to test and run Sogen, a Windows and Linux user space emulator, right in your browser."
        />
        <div className="h-dvh flex flex-col">
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

                    if (this.state.settings.mode !== s.mode) {
                      this.state.emulator?.stop();
                      this.resetFilesystemState();
                      this.setState({
                        settings: s,
                        emulator: undefined,
                        emulationStatus: undefined,
                      });
                      return;
                    }

                    this.setState({ settings: s });
                  }}
                />
              </PopoverContent>
            </Popover>

            <Button
              disabled={
                (!this.output.current && !this.macosTrace.current) ||
                !this.state.emulator ||
                this.state.emulator.getState() == EmulationState.Running
              }
              size="sm"
              title="Export Log"
              variant="secondary"
              className="fancy"
              onClick={() => this.exportLog()}
            >
              <FileEarmarkArrowDownFill />{" "}
              <span className="hidden sm:inline">Export Log</span>
            </Button>

            <Button
              disabled={!this.state.emulator || !this.isEmulatorPaused()}
              size="sm"
              title={
                this.isEmulatorPaused()
                  ? "Memory View"
                  : "Pause emulation to inspect memory"
              }
              variant={this.state.memoryViewOpen ? "default" : "secondary"}
              className="fancy"
              onClick={() =>
                this.setState({ memoryViewOpen: !this.state.memoryViewOpen })
              }
            >
              <Memory /> <span className="hidden sm:inline">Memory</span>
            </Button>

            <Button
              disabled={
                !!this.state.emulator &&
                this.state.emulator.getState() === EmulationState.Running
              }
              size="sm"
              title={
                !this.state.emulator || this.isEmulatorPaused()
                  ? "Debugger"
                  : "Pause emulation to debug"
              }
              variant={this.state.debuggerOpen ? "default" : "secondary"}
              className="fancy"
              onClick={() =>
                this.setState({ debuggerOpen: !this.state.debuggerOpen })
              }
            >
              <BugFill /> <span className="hidden sm:inline">Debugger</span>
            </Button>

            {this._renderFilesystemDrawer()}

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
          <div className="flex flex-1 min-h-0">
            <div className="relative flex flex-1 flex-col min-w-0">
              <EmulationSummary
                status={this.state.emulationStatus}
                executionTimeFetcher={this.fetchExecutionTime}
              />
              <div className="flex flex-1 flex-col pl-1 overflow-auto min-w-0">
                {this.state.settings.mode === "macos" ? (
                  <MacosTraceView
                    ref={this.macosTrace}
                    emulator={this.state.emulator}
                  />
                ) : (
                  <Output ref={this.output} />
                )}
              </div>
            </div>
            {(() => {
              const isMacos = this.state.settings.mode === "macos";
              const panelOpen = isMacos || this.state.uiWindowCount > 0;

              return (
                <div
                  className={
                    panelOpen
                      ? "relative flex h-full shrink-0 flex-col border-l bg-background"
                      : "relative flex h-full shrink-0 flex-col overflow-hidden border-l-0 bg-background"
                  }
                  style={{
                    width: panelOpen ? `${this.state.uiPanelWidth}px` : "0px",
                    maxWidth: "100vw",
                  }}
                >
                  <div
                    onMouseDown={(e) => {
                      e.preventDefault();
                      this.uiPanelDragging = true;
                      document.body.style.userSelect = "none";
                    }}
                    title="Drag to resize"
                    className={
                      panelOpen
                        ? "absolute inset-y-0 left-0 z-20 w-1.5 cursor-col-resize hover:bg-primary/40"
                        : "hidden"
                    }
                  />
                  {isMacos ? (
                    <div className="flex h-full min-h-0 flex-col">
                      <div className="p-2 pb-0">
                        <MacosAttach
                          disabled={
                            !!this.state.emulator &&
                            !isFinalState(this.state.emulator.getState())
                          }
                          onAttach={(macosAttachment) =>
                            this.setState({ macosAttachment })
                          }
                          onRun={(entry) => this.startEmulator(entry)}
                        />
                      </div>
                      <div className="min-h-0 flex-1">
                        <MacosGuestScreen
                          emulator={this.state.emulator}
                          settings={this.state.settings}
                          onRunDemo={(guestPath, needsRoot) =>
                            this.startEmulator(guestPath, {
                              attachRoot: needsRoot,
                            })
                          }
                        />
                      </div>
                    </div>
                  ) : (
                    <div className="flex flex-1 flex-col p-2 min-h-0">
                      <div className="flex-1 rounded-md border bg-muted/20 p-2 min-h-0">
                        <canvas
                          ref={this.uiCanvas}
                          width={960}
                          height={640}
                          className="h-full w-full rounded bg-background outline-none"
                        />
                      </div>
                    </div>
                  )}
                </div>
              );
            })()}
            {this.state.memoryViewOpen && this.state.emulator && (
              <MemoryView
                emulator={this.state.emulator}
                paused={!!this.isEmulatorPaused()}
                onClose={() => this.setState({ memoryViewOpen: false })}
              />
            )}
            {this.state.debuggerOpen && this.state.emulator && (
              <DebuggerView
                emulator={this.state.emulator}
                paused={!!this.isEmulatorPaused()}
                onClose={() => this.setState({ debuggerOpen: false })}
              />
            )}
          </div>
        </div>
      </>
    );
  }
}
