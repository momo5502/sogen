import { parse } from "shell-quote";

export interface Settings {
  verbose: boolean;
  concise: boolean;
  silent: boolean;
  bufferStdout: boolean;
  persist: boolean;
  execAccess: boolean;
  foreignAccess: boolean;
  wasm64: boolean;
  instructionSummary: boolean;
  ignoredFunctions: string[];
  interestingModules: string[];
  commandLine: string;
}

export interface TranslatedSettings {
  emulatorOptions: string[];
  applicationOptions: string[];
}

export function createDefaultSettings(): Settings {
  return {
    verbose: false,
    concise: false,
    silent: false,
    bufferStdout: true,
    persist: false,
    execAccess: true,
    foreignAccess: false,
    wasm64: false,
    instructionSummary: false,
    ignoredFunctions: [],
    interestingModules: [],
    commandLine: "",
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

  if (settings.verbose) {
    switches.push("-v");
  }

  if (settings.concise) {
    switches.push("-c");
  }

  if (settings.silent) {
    switches.push("-s");
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
