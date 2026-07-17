import { parse } from "shell-quote";

export type EmulatorMode = "windows" | "linux";

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

  if (settings.mode !== "linux") {
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
