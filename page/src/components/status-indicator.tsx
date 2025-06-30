import { Badge } from "@/components/ui/badge";
import { CircleFill } from "react-bootstrap-icons";
import { EmulationState as State } from "@/emulator";

function getStateName(state: State) {
  switch (state) {
    case State.Stopped:
      return "Stopped";
    case State.Paused:
      return "Paused";
    case State.Running:
      return "Running";
    case State.Failed:
      return "Failed";
    case State.Success:
      return "Success";
    default:
      return "";
  }
}

function getStateColor(state: State) {
  switch (state) {
    case State.Failed:
      return "bg-orange-600";
    case State.Paused:
      return "bg-amber-500";
    case State.Success:
      return "bg-lime-600";
    case State.Stopped:
      return "bg-yellow-800";
    case State.Running:
      return "bg-sky-500";
    default:
      return "";
  }
}

function getStateEmoji(state: State) {
  switch (state) {
    case State.Stopped:
      return "🟤";
    case State.Paused:
      return "🟡";
    case State.Running:
      return "🔵";
    case State.Failed:
      return "🔴";
    case State.Success:
      return "🟢";
    default:
      return "";
  }
}

function getFilename(path: string) {
  const lastSlash = path.lastIndexOf("/");
  if (lastSlash == -1) {
    return path;
  }

  return path.substring(lastSlash + 1);
}

export interface StatusIndicatorProps {
  state: State;
  application: string | undefined;
}

export function StatusIndicator(props: StatusIndicatorProps) {
  if (props.application && props.application.length > 0) {
    document.title = `${getStateEmoji(props.state)} ${getFilename(props.application)} | Sogen`;
  }

  return (
    <Badge variant="outline">
      <CircleFill
        className={
          getStateColor(props.state) +
          " rounded-full mr-1 n  duration-200 ease-in-out"
        }
        color="transparent"
      />
      {getStateName(props.state)}
    </Badge>
  );
}
