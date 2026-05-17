import React from "react";

interface ErrorBoundaryProps {
  children: React.ReactNode;
  label?: string;
}

interface ErrorBoundaryState {
  error: Error | null;
}

// Contains render errors to a single panel instead of unmounting the whole
// React tree (which left only a gray background).
export class ErrorBoundary extends React.Component<
  ErrorBoundaryProps,
  ErrorBoundaryState
> {
  constructor(props: ErrorBoundaryProps) {
    super(props);
    this.state = { error: null };
  }

  static getDerivedStateFromError(error: Error): ErrorBoundaryState {
    return { error };
  }

  componentDidCatch(error: Error, info: React.ErrorInfo) {
    console.error("Panel error:", error, info);
  }

  render() {
    if (this.state.error) {
      return (
        <div className="flex h-full flex-col items-start gap-2 overflow-auto p-3 font-mono text-xs text-muted-foreground">
          <span className="text-red-400">
            {this.props.label ?? "Panel"} crashed — contained (UI kept alive).
          </span>
          <pre className="whitespace-pre-wrap">{this.state.error.message}</pre>
          <button
            className="rounded border px-2 py-1 hover:bg-accent"
            onClick={() => this.setState({ error: null })}
          >
            Retry
          </button>
        </div>
      );
    }
    return this.props.children;
  }
}
