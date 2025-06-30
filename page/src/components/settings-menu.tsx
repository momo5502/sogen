import React from "react";
import { Checkbox } from "./ui/checkbox";
import { Label } from "./ui/label";

import { Settings } from "@/settings";

interface SettingsMenuProps {
  settings: Settings;
  onChange: (settings: Settings) => void;
}

export class SettingsMenu extends React.Component<SettingsMenuProps, Settings> {
  constructor(props: SettingsMenuProps) {
    super(props);
    this.getSettings = this.getSettings.bind(this);
    this.state = props.settings;
  }

  getSettings() {
    return this.state;
  }

  updateSettings(settings: Settings) {
    this.setState(() => settings);
  }

  componentDidUpdate(_: SettingsMenuProps, oldSettings: Settings) {
    if (JSON.stringify(oldSettings) !== JSON.stringify(this.state)) {
      this.props.onChange(this.state);
    }
  }

  render() {
    return (
      <div className="grid gap-4">
        <div className="space-y-2">
          <h4 className="font-medium leading-none">Settings</h4>
          <p className="text-sm text-muted-foreground">
            Set the settings for the emulation.
          </p>
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-verbose"
            checked={this.state.verbose}
            onCheckedChange={(checked: boolean) => {
              this.setState({ verbose: checked });
            }}
          />
          <Label htmlFor="settings-verbose">Verbose Logging</Label>
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-concise"
            checked={this.state.concise}
            onCheckedChange={(checked: boolean) => {
              this.setState({ concise: checked });
            }}
          />
          <Label htmlFor="settings-concise">Concise Logging</Label>
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-silent"
            checked={this.state.silent}
            onCheckedChange={(checked: boolean) => {
              this.setState({ silent: checked });
            }}
          />
          <Label htmlFor="settings-silent">Silent Logging</Label>
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-buffer"
            checked={this.state.bufferStdout}
            onCheckedChange={(checked: boolean) => {
              this.setState({ bufferStdout: checked });
            }}
          />
          <Label htmlFor="settings-buffer">Buffer stdout</Label>
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-exec"
            checked={this.state.execAccess}
            onCheckedChange={(checked: boolean) => {
              this.setState({ execAccess: checked });
            }}
          />
          <Label htmlFor="settings-exec">Log exec Memory Access</Label>
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-persist"
            checked={this.state.persist}
            onCheckedChange={(checked: boolean) => {
              this.setState({ persist: checked });
            }}
          />
          <Label htmlFor="settings-persist">Persist filesystem</Label>
        </div>
      </div>
    );
  }
}
