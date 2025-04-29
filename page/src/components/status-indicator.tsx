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
    default:
      return "";
  }
}

function getStateColor(state: State) {
  switch (state) {
    case State.Stopped:
      return "bg-orange-600";
    case State.Paused:
      return "bg-amber-500";
    case State.Running:
      return "bg-lime-600";
    default:
      return "";
  }
}

export interface StatusIndicatorProps {
  state: State;
}

export function StatusIndicator(props: StatusIndicatorProps) {
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
