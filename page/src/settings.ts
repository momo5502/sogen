export interface Settings {
  verbose: boolean;
  concise: boolean;
  silent: boolean;
  bufferStdout: boolean;
  persist: boolean;
  execAccess: boolean;
}

export function createDefaultSettings(): Settings {
  return {
    verbose: false,
    concise: false,
    silent: false,
    bufferStdout: true,
    persist: false,
    execAccess: true,
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

export function translateSettings(settings: Settings): string[] {
  const switches: string[] = [];

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

  return switches;
}
