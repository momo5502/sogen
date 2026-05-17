import { parse } from "shell-quote";

export type EmulatorMode = "windows" | "linux";

export interface Settings {
  logging: "verbose" | "silent" | "concise" | string;
  bufferStdout: boolean;
  persist: boolean;
  execAccess: boolean;
  foreignAccess: boolean;
  wasm64: boolean;
  instructionSummary: boolean;
  ignoredFunctions: string[];
  interestingModules: string[];
  commandLine: string;
  mode: EmulatorMode;
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
    commandLine: "",
    mode: "windows",
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
    // Linux emulator only supports --verbose
    if (settings.logging === "verbose") {
      switches.push("--verbose");
    }
  } else {
    // Windows emulator options
    switch (settings.logging) {
      case "verbose":
        switches.push("-v");
        break;
      case "silent":
        switches.push("-s");
        break;
      case "concise":
        switches.push("-c");
        break;

      default:
        break;
    }

    if (settings.bufferStdout) {
      switches.push("-b");
    }

    if (settings.execAccess) {
      switches.push("-x");
    }

    if (settings.foreignAccess) {
      switches.push("-f");
    }

    if (settings.instructionSummary) {
      switches.push("-is");
    }

    settings.ignoredFunctions.forEach((f) => {
      switches.push("-i");
      switches.push(f);
    });

    settings.interestingModules.forEach((m) => {
      switches.push("-m");
      switches.push(m);
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
