import { EmulationStatus } from "@/emulator";
import { TextTooltip } from "./text-tooltip";
import { BarChartSteps, CpuFill } from "react-bootstrap-icons";

import "./emulation-summary.css";

export interface EmulationSummaryProps {
  status?: EmulationStatus;
}

export function EmulationSummary(props: EmulationSummaryProps) {
  if (!props.status) {
    return <></>;
  }

  return (
    <div className="emulation-summary items-center absolute z-49 right-0 m-6 rounded-xl min-w-[150px] p-3 text-whtie cursor-default font-medium text-right text-sm whitespace-nowrap leading-6 font-mono">
      <TextTooltip tooltip={"Active threads"}>
        {props.status.activeThreads}
        <BarChartSteps className="inline ml-3" />
      </TextTooltip>
      <br />
      <TextTooltip tooltip={"Executed instructions"}>
        {props.status.executedInstructions.toLocaleString()}
        <CpuFill className="inline ml-3" />
      </TextTooltip>
    </div>
  );
}
