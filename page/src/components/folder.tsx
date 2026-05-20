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
import { TextTooltip } from "./text-tooltip";

export enum FolderElementType {
  Folder = 0,
  File,
}

export interface FolderElement {
  name: string;
  type: FolderElementType;
}

type ClickHandler = (element: FolderElement) => void;
type CreateFolderHandler = () => void;
type RemoveElementHandler = (element: FolderElement) => void;
type RenameElementHandler = (element: FolderElement) => void;
type DownloadElementHandler = (element: FolderElement) => void;
type AddFilesHandler = () => void;
type IconReader = (element: FolderElement) => string | null;

export interface FolderProps {
  elements: FolderElement[];
  iconReader: IconReader;
  clickHandler: ClickHandler;
  createFolderHandler: CreateFolderHandler;
  removeElementHandler: RemoveElementHandler;
  renameElementHandler: RenameElementHandler;
  downloadElementHandler: DownloadElementHandler;
  addFilesHandler: AddFilesHandler;
}

function elementComparator(e1: FolderElement, e2: FolderElement) {
  if (e1.type != e2.type) {
    return e1.type - e2.type;
  }

  return e1.name.localeCompare(e2.name);
}

function getIcon(
  element: FolderElement,
  iconReader: IconReader,
  className: string = "",
) {
  const icon = iconReader(element);
  if (icon) {
    return (
      <div className={className}>
        <div className="w-full h-full flex items-center">
          <img className="rounded-lg folder-icon" src={icon} />
        </div>
      </div>
    );
  }

  switch (element.type) {
    case FolderElementType.File:
      if (element.name.endsWith(".dll")) {
        return <FileEarmarkBinary className={className} />;
      }
      if (
        element.name.endsWith(".so") ||
        element.name.endsWith(".elf") ||
        element.name.endsWith(".bin")
      ) {
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

function renderIcon(element: FolderElement, iconReader: IconReader) {
  let className = "w-11 h-11 flex-1";
  return getIcon(element, iconReader, className);
}

function renderElement(element: FolderElement, props: FolderProps) {
  return (
    <div
      onClick={() => props.clickHandler(element)}
      className="folder-element cursor-default select-none flex flex-col gap-2 items-center text-center text-xs p-2 m-2 w-27 h-25 rounded-lg border bg-background shadow-xs hover:bg-accent hover:text-accent-foreground dark:bg-input/30 dark:border-input dark:hover:bg-input/50"
    >
      {renderIcon(element, props.iconReader)}
      <span className="whitespace-nowrap text-ellipsis overflow-hidden w-20">
        {element.name}
      </span>
    </div>
  );
}

export function trimFilename(filename: string, limit = 25) {
  if (limit < 4) {
    limit = 4;
  }

  if (filename.length < limit) {
    return filename;
  }

  return filename.substring(0, limit - 3) + "...";
}

function renderElementWithContext(element: FolderElement, props: FolderProps) {
  if (element.name == "..") {
    return renderElement(element, props);
  }

  return (
    <ContextMenu>
      <ContextMenuTrigger>
        <TextTooltip tooltip={element.name}>
          {renderElement(element, props)}
        </TextTooltip>
      </ContextMenuTrigger>
      <ContextMenuContent>
        <ContextMenuLabel>{trimFilename(element.name)}</ContextMenuLabel>
        <ContextMenuSeparator />
        {element.type != FolderElementType.File ? (
          <></>
        ) : (
          <ContextMenuItem
            onClick={() => props.downloadElementHandler(element)}
          >
            Download
          </ContextMenuItem>
        )}
        <ContextMenuItem onClick={() => props.renameElementHandler(element)}>
          Rename
        </ContextMenuItem>
        <ContextMenuItem onClick={() => props.removeElementHandler(element)}>
          Delete
        </ContextMenuItem>
      </ContextMenuContent>
    </ContextMenu>
  );
}

function renderElementWrapper(element: FolderElement, props: FolderProps) {
  return (
    <div key={`folder-element-${element.name}`}>
      {renderElementWithContext(element, props)}
    </div>
  );
}

export function Folder(props: FolderProps) {
  return (
    <ContextMenu>
      <ContextMenuTrigger>
        <ScrollArea className="h-[50dvh]">
          <div className="folder flex flex-wrap h-full text-neutral-300">
            {props.elements
              .sort(elementComparator)
              .map((e) => renderElementWrapper(e, props))}
          </div>
        </ScrollArea>
      </ContextMenuTrigger>
      <ContextMenuContent>
        <ContextMenuItem onClick={props.createFolderHandler}>
          Create new Folder
        </ContextMenuItem>
        <ContextMenuItem onClick={props.addFilesHandler}>
          Add Files
        </ContextMenuItem>
      </ContextMenuContent>
    </ContextMenu>
  );
}
