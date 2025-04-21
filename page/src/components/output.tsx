import React from 'react';
import { FixedSizeList as List } from 'react-window';

interface OutputProps { }

interface ColorState {
    color: string;
}

interface OutputState extends ColorState {
    lines: LogLine[];
}

interface FullOutputState extends OutputState {
    grouper: OutputGrouper;
    height: number,
    width: number,
}

interface LogLine {
    text: string;
    classNames: string;
};

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
        const cleanedString = str.replace(pattern, '');
        return [cleanedString, terminalValue];
    }

    return [str, color];
}

function extractColor(line: string, colorState: ColorState) {
    while (true) {
        const newLine = removeSubstringFromStart(line, "</span>");
        if (newLine == line) {
            break
        }

        line = newLine;
        colorState.color = '';
    }

    const [nextLine, color] = removeSpanFromStart(line, colorState.color);

    const finalLine = removeSubstringFromEnd(nextLine, "</span>");
    if (finalLine != nextLine) {
        colorState.color = '';
    } else {
        colorState.color = color;
    }

    return [finalLine, color];
}

function renderLine(line: string, colorState: ColorState) {
    const [newLine, color] = extractColor(line, colorState);

    return {
        text: newLine,
        classNames: 'whitespace-nowrap block ' + color
    };
}

function renderLines(lines: string[], color: string): OutputState {
    var state: ColorState = {
        color
    };

    const resultLines = lines.map(line => renderLine(line, state));

    return {
        lines: resultLines,
        color: state.color,
    };
}

function mergeLines(previousState: OutputState, newLines: string[]): OutputState {
    const result = renderLines(newLines, previousState.color);
    return { lines: previousState.lines.concat(result.lines), color: result.color };
}

class OutputGrouper {
    private lines: string[];
    private flushQueued: boolean;
    handler: (lines: string[]) => void;

    constructor() {
        this.lines = [];
        this.flushQueued = false;
        this.handler = () => { };
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

export class Output extends React.Component<OutputProps, FullOutputState> {
    private outputRef: React.RefObject<HTMLDivElement | null>;
    private listRef: React.RefObject<List | null>;
    private resizeObserver: ResizeObserver;

    constructor(props: OutputProps) {
        super(props);

        this.clear = this.clear.bind(this);
        this.logLine = this.logLine.bind(this);
        this.logLines = this.logLines.bind(this);
        this.updateDimensions = this.updateDimensions.bind(this);

        this.outputRef = React.createRef();
        this.listRef = React.createRef();
        this.resizeObserver = new ResizeObserver(this.updateDimensions);

        this.state = {
            lines: [],
            color: '',
            grouper: new OutputGrouper(),
            height: 10,
            width: 10,
        };

        this.state.grouper.handler = (lines: string[]) => {
            this.setState((s) => mergeLines(s, lines));
        };
    }

    componentDidMount() {
        this.updateDimensions();

        if (this.outputRef.current) {
            this.resizeObserver.observe(this.outputRef.current);
        }
    }

    componentWillUnmount() {
        this.resizeObserver.disconnect();
    }

    componentDidUpdate(_: OutputProps, prevState: FullOutputState) {
        if (prevState.lines.length == this.state.lines.length || !this.listRef.current) {
            return;
        }

        this.listRef.current.scrollToItem(this.state.lines.length - 1);
    }

    clear() {
        this.state.grouper.clear();
        this.setState({
            lines: [],
            color: '',
        });
    }

    updateDimensions() {
        if (!this.outputRef.current) {
            return;
        }

        this.setState({
            width: this.outputRef.current.offsetWidth,
            height: this.outputRef.current.offsetHeight,
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
            <div className='terminal-output' ref={this.outputRef}>
                <List ref={this.listRef}
                    width={this.state.width}
                    height={this.state.height}
                    itemCount={this.state.lines.length}
                    itemSize={20}>
                    {({ index, style }) => {
                        const line = this.state.lines[index];
                        return (
                            <span className={line.classNames} style={style}>
                                {line.text}
                            </span>
                        )
                    }}
                </List>
            </div>
        );
    }
}
