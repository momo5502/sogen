import {
  FolderFill,
  FolderSymlinkFill,
  FileEarmark,
  FiletypeExe,
  FileEarmarkBinary,
} from "react-bootstrap-icons";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuTrigger,
  ContextMenuSeparator,
  ContextMenuLabel,
} from "@/components/ui/context-menu";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";

export enum FolderElementType {
  Folder = 0,
  File,
}

export interface FolderElement {
  name: string;
  type: FolderElementType;
}

type ClickHandler = (element: FolderElement) => void;

export interface FolderProps {
  elements: FolderElement[];
  clickHandler: ClickHandler;
  //deleteHandler: (element: FolderElement) => void;
  //renameHandler: (element: FolderElement, name: string) => void;
}

function elementComparator(e1: FolderElement, e2: FolderElement) {
  if (e1.type != e2.type) {
    return e1.type - e2.type;
  }

  return e1.name.localeCompare(e2.name);
}

function getIcon(element: FolderElement, className: string = "") {
  switch (element.type) {
    case FolderElementType.File:
      if (element.name.endsWith(".dll")) {
        return <FileEarmarkBinary className={className} />;
      }
      if (element.name.endsWith(".exe")) {
        return <FiletypeExe className={className} />;
      }
      return <FileEarmark className={className} />;
    case FolderElementType.Folder:
      return element.name == ".." ? (
        <FolderSymlinkFill className={className} />
      ) : (
        <FolderFill className={className} />
      );
    default:
      return <></>;
  }
}

function renderIcon(element: FolderElement) {
  let className = "w-10 h-10";
  return getIcon(element, className);
}

function renderElement(element: FolderElement, clickHandler: ClickHandler) {
  return (
    <div
      key={`folder-element-${element.name}`}
      onClick={() => clickHandler(element)}
      className="folder-element select-none flex flex-col gap-4 items-center text-center p-4 m-4 w-30 h-25 rounded-lg border bg-background shadow-xs hover:bg-accent hover:text-accent-foreground dark:bg-input/30 dark:border-input dark:hover:bg-input/50"
    >
      {renderIcon(element)}
      <span className="whitespace-nowrap text-ellipsis overflow-hidden w-24">
        {element.name}
      </span>
    </div>
  );
}

function renderElementWithContext(
  element: FolderElement,
  clickHandler: ClickHandler,
) {
  if (element.name == "..") {
    return renderElement(element, clickHandler);
  }

  return (
    <ContextMenu>
      <ContextMenuTrigger>
        <Tooltip>
          <TooltipTrigger asChild>
            {renderElement(element, clickHandler)}
          </TooltipTrigger>
          <TooltipContent>
            <p>{element.name}</p>
          </TooltipContent>
        </Tooltip>
      </ContextMenuTrigger>
      <ContextMenuContent>
        <ContextMenuLabel inset>{element.name}</ContextMenuLabel>
        <ContextMenuSeparator />
        <ContextMenuItem>Rename</ContextMenuItem>
        <ContextMenuItem>Delete</ContextMenuItem>
      </ContextMenuContent>
    </ContextMenu>
  );
}

export function Folder(props: FolderProps) {
  return (
    <ScrollArea className="h-[50dvh]">
      <TooltipProvider delayDuration={700}>
        <ContextMenu>
          <ContextMenuTrigger>
            <div className="folder flex flex-wrap">
              {props.elements
                .sort(elementComparator)
                .map((e) => renderElementWithContext(e, props.clickHandler))}
            </div>
          </ContextMenuTrigger>
          <ContextMenuContent>
            <ContextMenuItem>Create new folder</ContextMenuItem>
          </ContextMenuContent>
        </ContextMenu>
      </TooltipProvider>
    </ScrollArea>
  );
}
