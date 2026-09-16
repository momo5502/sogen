import { parse } from "shell-quote";

export type EmulatorMode = "windows" | "linux" | "macos";

export interface EnvironmentVariable {
  name: string;
  value: string;
}

export interface Settings {
  logging: "verbose" | "silent" | "concise" | "very-concise" | string;
  bufferStdout: boolean;
  persist: boolean;
  execAccess: boolean;
  foreignAccess: boolean;
  wasm64: boolean;
  instructionSummary: boolean;
  ignoredFunctions: string[];
  interestingModules: string[];
  environmentVariables: EnvironmentVariable[];
  commandLine: string;
  mode: EmulatorMode;
  // macOS-only, mirroring src/macos-web/index.html's opt-desktop/opt-frame-interval/opt-maxinsn
  // defaults (1024x640 / 100ms / 0 == unlimited). Ignored by windows/linux's translateSettings branch.
  desktopSize: string;
  frameIntervalMs: number;
  maxInstructions: number;
  // macOS-only, mirroring opt-strlimit/opt-buflimit's defaults (256 / 32), which are also
  // src/macos-web/main.cpp's own web_options defaults when neither flag is sent.
  stringLimit: number;
  bufferLimit: number;
}

export interface TranslatedSettings {
  emulatorOptions: string[];
  applicationOptions: string[];
}

export function createDefaultSettings(): Settings {
  return {
    logging: "regular",
    bufferStdout: true,
    persist: false,
    execAccess: false,
    foreignAccess: false,
    wasm64: false,
    instructionSummary: false,
    ignoredFunctions: [],
    interestingModules: [],
    environmentVariables: [],
    commandLine: "",
    mode: "windows",
    desktopSize: "1024x640",
    frameIntervalMs: 100,
    maxInstructions: 0,
    stringLimit: 256,
    bufferLimit: 32,
  };
}

export function loadSettings(): Settings {
  const defaultSettings = createDefaultSettings();

  const settingsStr = localStorage.getItem("settings");
  if (!settingsStr) {
    return defaultSettings;
  }

  try {
    const userSettings = JSON.parse(settingsStr);
    const keys = Object.keys(defaultSettings);

    keys.forEach((k) => {
      if (k in userSettings) {
        (defaultSettings as any)[k] = userSettings[k];
      }
    });
  } catch (e) {}

  return defaultSettings;
}

export function saveSettings(settings: Settings) {
  localStorage.setItem("settings", JSON.stringify(settings));
}

export function translateSettings(settings: Settings): TranslatedSettings {
  const switches: string[] = [];
  const options: string[] = [];

  if (settings.mode === "linux") {
    if (settings.logging === "verbose") {
      switches.push("--verbose");
    }
  } else if (settings.mode === "macos") {
    // page/public/macos-emulator-worker.js runs src/macos-web/main.cpp, not the general analyzer's
    // macOS front-end (src/macos-analyzer/main.cpp): its flag surface is --no-decode/--no-syscalls/
    // --no-modules/--no-memory-report/--log/--gui/--desktop-size=/--frame-interval=/--ignore=/--env=, a
    // narrower set built for the browser build rather than the CLI11 flags (-c/-v/-s/--skip-syscalls/
    // --os=) the general front-end takes. The windows-shaped switches below do not exist there --
    // sending one is silently ignored rather than rejected, but still wrong to send.
    switch (settings.logging) {
      case "silent":
        switches.push("--no-syscalls", "--no-modules", "--no-memory-report");
        break;
      case "very-concise":
        switches.push("--no-decode", "--no-modules");
        break;
      case "concise":
        switches.push("--no-decode");
        break;

      // What -v does to the native macOS front-end (src/macos-analyzer/macos_analysis.cpp: --verbose is
      // what unmutes the emulator's internal logging). Off at every other level because it is dense --
      // an AppKit guest logs ~2,000 lines a second, most of them one mach_msg2 trace.
      case "verbose":
        switches.push("--log");
        break;

      default:
        break;
    }

    if (settings.ignoredFunctions.length) {
      // One comma-separated value, not a repeated flag: main.cpp's parse_options splits --ignore= on
      // commas (split_into), unlike windows/linux's --ignore NAME per function.
      switches.push(`--ignore=${settings.ignoredFunctions.join(",")}`);
    }

    settings.environmentVariables.forEach((variable) => {
      const name = variable.name.trim();
      if (!name) {
        return;
      }

      // One --env=NAME=VALUE per variable, not the two-token --env NAME VALUE windows/linux use below.
      switches.push(`--env=${name}=${variable.value}`);
    });

    // Always on: page/src/macos/guest-screen.tsx is macOS mode's only way to watch a run, and it is the
    // composed desktop -- there is no separate headless view to fall back to.
    switches.push("--gui");

    const desktopSize = settings.desktopSize.trim();
    if (/^\d+\s*[xX]\s*\d+$/.test(desktopSize)) {
      switches.push(`--desktop-size=${desktopSize.replace(/\s+/g, "")}`);
    }

    switches.push(
      `--frame-interval=${Math.max(0, settings.frameIntervalMs || 0)}`,
    );
    switches.push(
      `--max-instructions=${Math.max(0, settings.maxInstructions || 0)}`,
    );
    switches.push(`--string-limit=${Math.max(0, settings.stringLimit || 0)}`);
    switches.push(`--buffer-limit=${Math.max(0, settings.bufferLimit || 0)}`);
  } else {
    switch (settings.logging) {
      case "verbose":
        switches.push("--verbose");
        break;
      case "silent":
        switches.push("--silent");
        break;
      case "concise":
        switches.push("--concise");
        break;
      case "very-concise":
        switches.push("--very-concise");
        break;

      default:
        break;
    }

    if (settings.bufferStdout) {
      switches.push("--buffer");
    }

    if (settings.execAccess) {
      switches.push("--exec");
    }

    if (settings.foreignAccess) {
      switches.push("--foreign");
    }

    if (settings.instructionSummary) {
      switches.push("--inst-summary");
    }

    settings.ignoredFunctions.forEach((f) => {
      switches.push("--ignore");
      switches.push(f);
    });

    settings.interestingModules.forEach((m) => {
      switches.push("--module");
      switches.push(m);
    });
  }

  if (settings.mode === "windows") {
    settings.environmentVariables.forEach((variable) => {
      const name = variable.name.trim();
      if (!name) {
        return;
      }

      switches.push("--env");
      switches.push(name);
      switches.push(variable.value);
    });
  }
  try {
    const argv = parse(settings.commandLine) as string[];
    options.push(...argv);
  } catch (e) {
    console.log(e);
  }

  return {
    applicationOptions: options,
    emulatorOptions: switches,
  };
}
