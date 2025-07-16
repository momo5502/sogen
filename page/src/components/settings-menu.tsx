import React from "react";
import { Checkbox } from "./ui/checkbox";
import { Label } from "./ui/label";

import { Settings } from "@/settings";
import { TextTooltip } from "./text-tooltip";

interface SettingsMenuProps {
  settings: Settings;
  allowWasm64: boolean;
  onChange: (settings: Settings) => void;
}

interface SettingsLabelProps {
  htmlFor?: string | undefined;
  text: React.ReactNode;
  tooltip: React.ReactNode;
}

function SettingsLabel(props: SettingsLabelProps) {
  return (
    <Label htmlFor={props.htmlFor}>
      <TextTooltip tooltip={props.tooltip}>{props.text}</TextTooltip>
    </Label>
  );
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
          <SettingsLabel
            htmlFor="settings-verbose"
            text={"Verbose Logging"}
            tooltip={"Very detailed logging of all function call and accesses"}
          />
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-concise"
            checked={this.state.concise}
            onCheckedChange={(checked: boolean) => {
              this.setState({ concise: checked });
            }}
          />
          <SettingsLabel
            htmlFor="settings-concise"
            text={"Concise Logging"}
            tooltip={"Suppress logging until the application code runs"}
          />
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-silent"
            checked={this.state.silent}
            onCheckedChange={(checked: boolean) => {
              this.setState({ silent: checked });
            }}
          />
          <SettingsLabel
            htmlFor="settings-silent"
            text={"Silent Logging"}
            tooltip={"Suppress all logging except for stdout"}
          />
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-buffer"
            checked={this.state.bufferStdout}
            onCheckedChange={(checked: boolean) => {
              this.setState({ bufferStdout: checked });
            }}
          />
          <SettingsLabel
            htmlFor="settings-buffer"
            text={"Buffer stdout"}
            tooltip={
              "Group stdout and print everything when the emulation ends"
            }
          />
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-exec"
            checked={this.state.execAccess}
            onCheckedChange={(checked: boolean) => {
              this.setState({ execAccess: checked });
            }}
          />
          <SettingsLabel
            htmlFor="settings-exec"
            text={"Log exec Memory Access"}
            tooltip={"Log when the application reads/writes executable memory"}
          />
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-persist"
            checked={this.state.persist}
            onCheckedChange={(checked: boolean) => {
              this.setState({ persist: checked });
            }}
          />
          <SettingsLabel
            htmlFor="settings-persist"
            text={"Persist filesystem"}
            tooltip={
              "Persist files and folders that were created, modified or deleted during the emulation"
            }
          />
        </div>

        <div className="flex gap-6">
          <Checkbox
            id="settings-mem64"
            disabled={!this.props.allowWasm64}
            checked={this.state.wasm64}
            onCheckedChange={(checked: boolean) => {
              this.setState({ wasm64: checked });
            }}
          />
          <SettingsLabel
            htmlFor="settings-mem64"
            text={"64-Bit WebAssembly"}
            tooltip={
              "Use 64-bit WebAssembly which supports emulating applications that require more than 2gb of memory"
            }
          />
        </div>
      </div>
    );
  }
}
