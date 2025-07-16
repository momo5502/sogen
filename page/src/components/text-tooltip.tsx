import {
  Tooltip,
  TooltipContent,
  TooltipTrigger,
} from "@/components/ui/tooltip";

export interface TextTooltipProps {
  children?: React.ReactNode;
  tooltip: React.ReactNode;
}

export function TextTooltip(props: TextTooltipProps) {
  return (
    <Tooltip delayDuration={700} disableHoverableContent>
      <TooltipTrigger asChild>
        <span>{props.children}</span>
      </TooltipTrigger>
      <TooltipContent>{props.tooltip}</TooltipContent>
    </Tooltip>
  );
}
