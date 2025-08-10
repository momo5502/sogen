import { Input } from "./ui/input";
import { Button } from "./ui/button";
import { Plus, Trash } from "react-bootstrap-icons";
import { Label } from "./ui/label";

interface ItemListProps {
  title: string;
  items: string[];
  onChange: (items: string[]) => void;
}

export function ItemList(props: ItemListProps) {
  const removeItem = (index: number) => {
    const newItems = [...props.items];
    newItems.splice(index, 1);
    props.onChange(newItems);
  };

  const addItem = (item: string) => {
    if (item.length == 0) {
      return;
    }

    const newItems = props.items.concat(item);
    props.onChange(newItems);
  };

  return (
    <div className="grid gap-3">
      <div className="space-y-2">
        <h4 className="font-medium leading-none">{props.title}</h4>
        {/*<p className="text-sm text-muted-foreground">
          Set the settings for the emulation.
        </p>*/}
      </div>

      <div className="grid gap-2 overflow-auto overflow-x-hidden max-h-45 mt-2 mb-2">
        {props.items.map((item, index) => {
          return (
            <div
              key={`item-list-item-${index}-${item}`}
              className="flex gap-3 items-center min-w-0"
            >
              <Label className="flex-1 text-left truncate min-w-0">
                {item}
              </Label>
              <Button
                onClick={() => removeItem(index)}
                variant="ghost"
                size="sm"
                className="fancy rounded-lg"
              >
                <Trash />
              </Button>
            </div>
          );
        })}
      </div>

      <form
        onSubmit={(e) => {
          const nameInput = (e.target as any).elements.name;
          const newItem = nameInput.value;
          nameInput.value = "";

          addItem(newItem);
          e.preventDefault();
        }}
      >
        <div className="flex gap-3 items-center">
          <Input id="name" />
          <Button
            type="submit"
            variant="secondary"
            className="fancy rounded-lg"
          >
            <Plus />
          </Button>
        </div>
      </form>
    </div>
  );
}
