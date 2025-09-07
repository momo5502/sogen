import React from "react";
import { List, ListImperativeAPI, type RowComponentProps } from "react-window";
import { ArrowDown } from "react-bootstrap-icons";
import { Button } from "./ui/button";

interface OutputProps {}

interface ColorState {
  color: string;
}

interface OutputState extends ColorState {
  lines: LogLine[];
}

enum SizeState {
  Final,
  Updating,
}

interface FullOutputState extends OutputState {
  grouper: OutputGrouper;
  height: number;
  width: number;
  state: SizeState;
  autoScroll: boolean;
}

interface LogLine {
  text: string;
  classNames: string;
}

function removeSubstringFromStart(str: string, substring: string): string {
  if (str.startsWith(substring)) {
    return str.slice(substring.length);
  }
  return str;
}

function removeSubstringFromEnd(str: string, substring: string): string {
  if (str.endsWith(substring)) {
    return str.slice(0, -substring.length);
  }
  return str;
}

function removeSpanFromStart(str: string, color: string) {
  const pattern = /^<span class="(terminal-[a-z-]+)">/;
  const match = str.match(pattern);

  if (match) {
    const terminalValue = match[1];
    const cleanedString = str.replace(pattern, "");
    return [cleanedString, terminalValue];
  }

  return [str, color];
}

function extractColor(line: string, colorState: ColorState) {
  while (true) {
    const newLine = removeSubstringFromStart(line, "</span>");
    if (newLine == line) {
      break;
    }

    line = newLine;
    colorState.color = "";
  }

  const [nextLine, color] = removeSpanFromStart(line, colorState.color);

  const finalLine = removeSubstringFromEnd(nextLine, "</span>");
  if (finalLine != nextLine) {
    colorState.color = "";
  } else {
    colorState.color = color;
  }

  return [finalLine, color];
}

function renderLine(line: string, colorState: ColorState) {
  const [newLine, color] = extractColor(line, colorState);

  return {
    text: newLine,
    classNames: "whitespace-nowrap block " + color,
  };
}

function renderLines(lines: string[], color: string): OutputState {
  var state: ColorState = {
    color,
  };

  const resultLines = lines.map((line) => renderLine(line, state));

  return {
    lines: resultLines,
    color: state.color,
  };
}

function mergeLines(
  previousState: OutputState,
  newLines: string[],
): OutputState {
  const result = renderLines(newLines, previousState.color);
  return {
    lines: previousState.lines.concat(result.lines),
    color: result.color,
  };
}

class OutputGrouper {
  private lines: string[];
  private flushQueued: boolean;
  handler: (lines: string[]) => void;

  constructor() {
    this.lines = [];
    this.flushQueued = false;
    this.handler = () => {};
  }

  clear() {
    this.lines = [];
    this.flushQueued = false;
  }

  flush() {
    const lines = this.lines;
    this.lines = [];
    this.handler(lines);
  }

  queueFlush() {
    if (this.flushQueued) {
      return false;
    }

    this.flushQueued = true;

    requestAnimationFrame(() => {
      if (!this.flushQueued) {
        return;
      }

      this.flushQueued = false;
      this.flush();
    });
  }

  storeLines(lines: string[]) {
    this.lines = this.lines.concat(lines);
    this.queueFlush();
  }
}

function LogLineRow({
  ariaAttributes,
  lines,
  index,
  style,
}: RowComponentProps<{
  lines: LogLine[];
}>) {
  {
    const line = lines[index];
    return (
      <span className={line.classNames} style={style} {...ariaAttributes}>
        {line.text}
      </span>
    );
  }
}

export class Output extends React.Component<OutputProps, FullOutputState> {
  private outputRef: React.RefObject<HTMLDivElement | null>;
  private listRef: React.RefObject<ListImperativeAPI | null>;
  private resizeObserver: ResizeObserver;
  private scrollElement: HTMLDivElement | null | undefined;

  constructor(props: OutputProps) {
    super(props);

    this.clear = this.clear.bind(this);
    this.logLine = this.logLine.bind(this);
    this.logLines = this.logLines.bind(this);
    this.handleScroll = this.handleScroll.bind(this);
    this.scrollListToEnd = this.scrollListToEnd.bind(this);
    this.updateDimensions = this.updateDimensions.bind(this);

    this.outputRef = React.createRef();
    this.listRef = React.createRef();
    this.resizeObserver = new ResizeObserver(this.updateDimensions);

    this.state = {
      lines: [],
      color: "",
      grouper: new OutputGrouper(),
      height: 10,
      width: 10,
      state: SizeState.Final,
      autoScroll: true,
    };

    this.state.grouper.handler = (lines: string[]) => {
      this.setState((s) => mergeLines(s, lines));
    };
  }

  handleScroll(e: Event) {
    const threshold = 40;
    const element = e.target as HTMLElement;
    const { scrollTop, scrollHeight, clientHeight } = element;
    const isAtEnd = scrollTop + clientHeight >= scrollHeight - threshold;

    this.setState({ autoScroll: isAtEnd });
  }

  unregisterScrollListener() {
    this.scrollElement?.removeEventListener("scroll", this.handleScroll);
  }

  registerScrollListener(element: HTMLDivElement | null | undefined) {
    if (element == this.scrollElement) {
      return;
    }

    this.unregisterScrollListener();
    this.scrollElement = element;
    element?.addEventListener("scroll", this.handleScroll);
  }

  registerScrollOnList() {
    this.registerScrollListener(this.listRef.current?.element);
  }

  componentDidMount() {
    this.updateDimensions();

    if (this.outputRef.current) {
      this.resizeObserver.observe(this.outputRef.current);
    }

    this.registerScrollOnList();
  }

  componentWillUnmount() {
    this.resizeObserver.disconnect();
    this.unregisterScrollListener();
  }

  scrollListToEnd() {
    if (this.listRef.current && this.state.lines.length > 0) {
      this.listRef.current.scrollToRow({
        index: this.state.lines.length - 1,
        behavior: "instant",
      });
    }

    this.setState({ autoScroll: true });
  }

  componentDidUpdate(_: OutputProps, prevState: FullOutputState) {
    this.registerScrollOnList();

    if (
      this.state.autoScroll &&
      prevState.lines.length != this.state.lines.length
    ) {
      this.scrollListToEnd();
    }
  }

  clear() {
    this.state.grouper.clear();
    this.setState({
      lines: [],
      color: "",
    });
  }

  updateDimensions() {
    if (!this.outputRef.current) {
      return;
    }

    if (this.state.state == SizeState.Updating) {
      this.setState({
        width: this.outputRef.current.offsetWidth - 1,
        height: this.outputRef.current.offsetHeight - 1,
        state: SizeState.Final,
      });

      return;
    }

    this.setState(
      {
        width: 0,
        height: 0,
        state: SizeState.Updating,
      },
      this.triggerDimensionUpdate.bind(this),
    );
  }

  triggerDimensionUpdate() {
    requestAnimationFrame(() => {
      this.updateDimensions();
    });
  }

  logLines(lines: string[]) {
    this.state.grouper.storeLines(lines);
  }

  logLine(line: string) {
    this.logLines([line]);
  }

  render() {
    return (
      <div className="terminal-output" ref={this.outputRef}>
        <List
          listRef={this.listRef}
          overscanCount={30}
          rowComponent={LogLineRow}
          rowCount={this.state.lines.length}
          rowProps={{ lines: this.state.lines }}
          rowHeight={20}
          style={{ height: this.state.height, width: this.state.width }}
        />
        <Button
          className={
            "absolute bottom-6 right-6 z-50 terminal-glass transition-opacity duration-50 ease-linear " +
            (this.state.autoScroll ? "opacity-0" : "")
          }
          variant="secondary"
          onClick={this.scrollListToEnd}
        >
          <ArrowDown />
        </Button>
      </div>
    );
  }
}
