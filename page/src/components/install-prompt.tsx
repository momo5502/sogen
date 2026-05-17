import { useState } from "react";
import Installer from "../Installer";
import { Button } from "./ui/button";
import { TextTooltip } from "./text-tooltip";

export function InstallPrompt() {
  const [hidden, setHidden] = useState(false);

  if (hidden || Installer.isInstalled() || Installer.isRejected()) {
    return <></>;
  }

  const hide = () => {
    setHidden(true);
  };

  const install = () => {
    Installer.install();
    hide();
  };

  const reject = () => {
    Installer.reject();
    hide();
  };

  return (
    <div className="terminal-glass items-center fixed z-49 bottom-0 left-0 m-6 rounded-xl min-w-[150px] p-3 text-white cursor-default font-medium text-right text-sm whitespace-nowrap leading-6">
      <TextTooltip
        tooltip={"Register Sogen as PWA so that it works fully offline"}
      >
        Install Sogen for offline use?
      </TextTooltip>
      <div className="flex gap-3 mt-3">
        <Button
          size="sm"
          variant="secondary"
          className="fancy flex-1"
          onClick={install}
        >
          Yes
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy flex-1"
          onClick={hide}
        >
          No
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy flex-1"
          onClick={reject}
        >
          Never
        </Button>
      </div>
    </div>
  );
}
