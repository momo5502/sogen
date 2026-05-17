import { Plus, Trash } from "react-bootstrap-icons";

import { Button } from "./ui/button";
import { Input } from "./ui/input";

import { EnvironmentVariable } from "@/settings";

interface EnvironmentVariableListProps {
  items: EnvironmentVariable[];
  onChange: (items: EnvironmentVariable[]) => void;
}

export function EnvironmentVariableList(props: EnvironmentVariableListProps) {
  const addItem = () => {
    props.onChange(
      props.items.concat({
        name: "",
        value: "",
      }),
    );
  };

  const removeItem = (index: number) => {
    const nextItems = [...props.items];
    nextItems.splice(index, 1);
    props.onChange(nextItems);
  };

  const updateItem = (
    index: number,
    field: keyof EnvironmentVariable,
    value: string,
  ) => {
    const nextItems = [...props.items];
    nextItems[index] = {
      ...nextItems[index],
      [field]: value,
    };
    props.onChange(nextItems);
  };

  return (
    <div className="grid gap-3">
      <h4 className="font-medium leading-none">Environment Variables</h4>

      <div className="flex flex-wrap items-center gap-2">
        {props.items.map((item, index) => {
          return (
            <div
              key={`env-var-${index}`}
              className="flex min-w-0 flex-1 basis-full items-center gap-2 rounded-xl border bg-card/60 px-2 py-2"
            >
              <Input
                value={item.name}
                onChange={(e) => updateItem(index, "name", e.target.value)}
                placeholder="key"
                className="h-7 min-w-0 flex-1 border-0 bg-muted/60 px-2 font-mono text-sm shadow-none focus-visible:ring-0"
              />
              <span className="text-sm text-muted-foreground">:</span>
              <Input
                value={item.value}
                onChange={(e) => updateItem(index, "value", e.target.value)}
                placeholder="value"
                className="h-7 min-w-0 flex-1 border-0 bg-muted/60 px-2 font-mono text-sm shadow-none focus-visible:ring-0"
              />
              <Button
                type="button"
                onClick={() => removeItem(index)}
                variant="ghost"
                size="sm"
                className="h-7 w-7 shrink-0 rounded-lg p-0"
              >
                <Trash />
              </Button>
            </div>
          );
        })}

        <Button
          type="button"
          variant="secondary"
          className="fancy h-8 w-8 rounded-lg p-0"
          onClick={addItem}
        >
          <Plus />
        </Button>
      </div>
    </div>
  );
}
