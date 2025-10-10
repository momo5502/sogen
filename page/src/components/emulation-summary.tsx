import { EmulationStatus } from "@/emulator";
import { TextTooltip } from "./text-tooltip";
import {
  BarChartSteps,
  CpuFill,
  FloppyFill,
  StopwatchFill,
} from "react-bootstrap-icons";
import React from "react";

export interface EmulationSummaryProps {
  status?: EmulationStatus;
  executionTimeFetcher: () => number;
}

function formatMemory(value: BigInt): string {
  const abbr = ["B", "KB", "MB", "GB", "PB"];

  let num = Number(value);
  let index = 0;

  while (num >= 1024 && index < abbr.length - 1) {
    num /= 1024;
    index++;
  }

  return num.toFixed(2) + " " + abbr[index];
}

function formatTime(seconds: number): string {
  const hrs = Math.floor(seconds / 3600);
  const mins = Math.floor((seconds % 3600) / 60);
  const secs = Math.floor(seconds % 60);

  const secsString = secs < 10 ? "0" + secs : secs.toString();

  if (hrs > 0) {
    const minsString = mins < 10 ? "0" + mins : mins.toString();
    return `${hrs.toString()}:${minsString}:${secsString}`;
  }

  return `${mins.toString()}:${secsString}`;
}

export class EmulationSummary extends React.Component<
  EmulationSummaryProps,
  {}
> {
  private timer: NodeJS.Timeout | undefined = undefined;

  constructor(props: EmulationSummaryProps) {
    super(props);
  }

  componentDidMount(): void {
    if (this.timer) {
      clearInterval(this.timer);
    }

    this.timer = setInterval(() => {
      this.forceUpdate();
    }, 200);
  }

  componentWillUnmount(): void {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = undefined;
    }
  }

  render() {
    if (!this.props.status) {
      return <></>;
    }

    return (
      <div className="emulation-summary terminal-glass items-center absolute z-49 right-0 m-6 rounded-xl min-w-[150px] p-3 text-white cursor-default font-medium text-right text-sm whitespace-nowrap leading-6 font-mono">
        <TextTooltip tooltip={"Active Threads"}>
          {this.props.status.activeThreads}
          <BarChartSteps className="inline ml-3" />
        </TextTooltip>
        <br />
        <TextTooltip tooltip={"Application Memory"}>
          {formatMemory(this.props.status.committedMemory)}
          <FloppyFill className="inline ml-3" />
        </TextTooltip>
        <br />
        <TextTooltip tooltip={"Executed Instructions"}>
          {this.props.status.executedInstructions.toLocaleString()}
          <CpuFill className="inline ml-3" />
        </TextTooltip>
        <br />
        <TextTooltip tooltip={"Execution Time"}>
          {formatTime(this.props.executionTimeFetcher() / 1000)}
          <StopwatchFill className="inline ml-3" />
        </TextTooltip>
      </div>
    );
  }
}
