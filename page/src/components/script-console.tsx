import React from "react";

import Editor from "@monaco-editor/react";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";
import { PlayFill, StopFill, PlusLg, Trash } from "react-bootstrap-icons";

import { Emulator } from "@/emulator";
import { runScript, type ScriptHandle } from "@/debugger/scripting";

const STORAGE_KEY = "sogen.debugger.scripts";

interface Script {
  id: string;
  name: string;
  source: string;
}

const DEFAULT_SOURCE = `// Scripting bridge — drives the REAL emulator via the debugger backend.
// 'emu' mirrors the sogen / emu.debug object model.
const regs = await emu.debug.registers();
print("rip = " + regs.rip);

for (const insn of await emu.debug.disassemble(BigInt(regs.rip), 5)) {
  print(insn.address + "  " + insn.mnemonic + " " + insn.operands);
}

// await emu.debug.breakpoint(0x140001000n);
// await emu.debug.step_into();
`;

function loadScripts(): Script[] {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Script[];
      if (Array.isArray(parsed) && parsed.length > 0) {
        return parsed;
      }
    }
  } catch {
    /* ignore */
  }
  return [{ id: "1", name: "script", source: DEFAULT_SOURCE }];
}

export interface ScriptConsoleProps {
  emulator: Emulator;
}

export function ScriptConsole({ emulator }: ScriptConsoleProps) {
  const [scripts, setScripts] = React.useState<Script[]>(loadScripts);
  const [activeId, setActiveId] = React.useState<string>(
    () => loadScripts()[0].id,
  );
  const [output, setOutput] = React.useState<string[]>([]);
  const [running, setRunning] = React.useState(false);
  const handleRef = React.useRef<ScriptHandle | null>(null);
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const editorRef = React.useRef<any>(null);

  const active = scripts.find((s) => s.id === activeId) ?? scripts[0];

  React.useEffect(() => {
    const t = setTimeout(() => {
      try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(scripts));
      } catch {
        /* ignore quota */
      }
    }, 300);
    return () => clearTimeout(t);
  }, [scripts]);

  const updateSource = (source: string) => {
    setScripts((prev) =>
      prev.map((s) => (s.id === active.id ? { ...s, source } : s)),
    );
  };

  const appendLine = React.useCallback((line: string) => {
    setOutput((prev) => {
      const next = prev.concat(line.split("\n"));
      return next.length > 2000 ? next.slice(next.length - 2000) : next;
    });
  }, []);

  const execute = async (source: string) => {
    if (running) {
      return;
    }
    const handle: ScriptHandle = { cancelled: false };
    handleRef.current = handle;
    setRunning(true);
    setOutput([`> running ${active.name}…`]);

    const result = await runScript(emulator, source, appendLine, handle);
    if (!result.ok) {
      appendLine("--- traceback ---");
      appendLine(result.error ?? "unknown error");
    } else {
      appendLine("> done");
    }
    setRunning(false);
    handleRef.current = null;
  };

  const runFull = () => execute(active.source);

  const runSelection = () => {
    const ed = editorRef.current;
    if (!ed) {
      return;
    }
    const sel = ed.getModel()?.getValueInRange(ed.getSelection());
    execute(sel && sel.trim().length > 0 ? sel : active.source);
  };

  const stop = () => {
    if (handleRef.current) {
      handleRef.current.cancelled = true;
    }
  };

  const addScript = () => {
    const id = String(Date.now());
    setScripts((prev) =>
      prev.concat({
        id,
        name: `script ${prev.length + 1}`,
        source: "// new script\n",
      }),
    );
    setActiveId(id);
  };

  const deleteScript = (id: string) => {
    setScripts((prev) => {
      const next = prev.filter((s) => s.id !== id);
      const result =
        next.length > 0
          ? next
          : [{ id: "1", name: "script", source: DEFAULT_SOURCE }];
      if (id === activeId) {
        setActiveId(result[0].id);
      }
      return result;
    });
  };

  return (
    <div className="flex h-full flex-col">
      <div className="flex items-center gap-1 border-b p-1">
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Run (full script)"
          disabled={running}
          onClick={runFull}
        >
          <PlayFill />
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Run selection"
          disabled={running}
          onClick={runSelection}
        >
          <PlayFill /> sel
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Stop"
          disabled={!running}
          onClick={stop}
        >
          <StopFill />
        </Button>
        <div className="mx-2 h-4 w-px bg-border" />
        <div className="flex flex-1 items-center gap-1 overflow-x-auto">
          {scripts.map((s) => (
            <button
              key={s.id}
              onClick={() => setActiveId(s.id)}
              className={cn(
                "shrink-0 rounded px-2 py-0.5 text-xs",
                s.id === active.id
                  ? "bg-accent"
                  : "text-muted-foreground hover:bg-accent/50",
              )}
            >
              {s.name}
            </button>
          ))}
          <Button
            size="sm"
            variant="ghost"
            className="h-6"
            title="New script"
            onClick={addScript}
          >
            <PlusLg />
          </Button>
        </div>
        <Input
          value={active.name}
          spellCheck={false}
          className="h-7 w-32 text-xs"
          onChange={(e) =>
            setScripts((prev) =>
              prev.map((s) =>
                s.id === active.id ? { ...s, name: e.target.value } : s,
              ),
            )
          }
        />
        <Button
          size="sm"
          variant="ghost"
          className="h-7"
          title="Delete script"
          onClick={() => deleteScript(active.id)}
        >
          <Trash />
        </Button>
      </div>

      <div className="min-h-0 flex-1">
        <Editor
          height="100%"
          language="javascript"
          theme="vs-dark"
          value={active.source}
          onChange={(v) => updateSource(v ?? "")}
          onMount={(ed) => {
            editorRef.current = ed;
          }}
          options={{
            minimap: { enabled: false },
            fontSize: 12,
            scrollBeyondLastLine: false,
            automaticLayout: true,
          }}
        />
      </div>

      <div className="h-40 shrink-0 overflow-auto border-t bg-black/30 p-2 font-mono text-[11px] whitespace-pre-wrap">
        {output.length === 0 ? (
          <span className="text-muted-foreground">
            Output. `emu.debug.*` drives the live emulator (mirrors the sogen
            Python API; runs as JS).
          </span>
        ) : (
          output.map((l, i) => <div key={i}>{l}</div>)
        )}
      </div>
    </div>
  );
}
