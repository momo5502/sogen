export interface Settings {
  verbose: boolean;
  concise: boolean;
  silent: boolean;
  bufferStdout: boolean;
  persist: boolean;
}

export function createDefaultSettings(): Settings {
  return {
    verbose: false,
    concise: false,
    silent: false,
    bufferStdout: true,
    persist: false,
  };
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

  return switches;
}
