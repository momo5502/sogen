import React from "react";
import { Folder, FolderElement, FolderElementType } from "./components/folder";
import { Filesystem } from "./filesystem";

import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Button } from "./components/ui/button";
import { Input } from "./components/ui/input";

export interface FilesystemExplorerProps {
  filesystem: Filesystem;
}
export interface FilesystemExplorerState {
  path: string[];
  createFolder: boolean;
  errorText: string;
}

function getFolderElements(filesystem: Filesystem, path: string[]) {
  const fullPath = "/root/filesys/" + path.join("/");
  const files = filesystem.readDir(fullPath);

  return files
    .filter((f) => {
      if (f == ".") {
        return false;
      }

      if (path.length == 0 && f == "..") {
        return false;
      }

      return true;
    })
    .map((f) => {
      const element: FolderElement = {
        name: f,
        type: filesystem.isFolder(`${fullPath}/${f}`)
          ? FolderElementType.Folder
          : FolderElementType.File,
      };

      return element;
    });
}

export class FilesystemExplorer extends React.Component<
  FilesystemExplorerProps,
  FilesystemExplorerState
> {
  constructor(props: FilesystemExplorerProps) {
    super(props);

    this._showError = this._showError.bind(this);
    this._onFolderCreate = this._onFolderCreate.bind(this);
    this._onElementSelect = this._onElementSelect.bind(this);
    this._showFolderCreateDialog = this._showFolderCreateDialog.bind(this);

    this.state = {
      path: [],
      createFolder: false,
      errorText: "",
    };
  }

  _showError(errorText: string) {
    this.setState({ errorText });
  }

  _onElementSelect(element: FolderElement) {
    if (element.type != FolderElementType.Folder) {
      return;
    }

    this.setState((s) => {
      const path = [...s.path];

      if (element.name == "..") {
        path.pop();
      } else {
        path.push(element.name);
      }

      return {
        path,
      };
    });
  }

  _showFolderCreateDialog() {
    this.setState({
      createFolder: true,
    });
  }

  async _onFolderCreate(name: string) {
    this.setState({
      createFolder: false,
    });

    name = name.toLowerCase();

    if (name.length == 0) {
      return;
    }

    if (name.includes("/") || name.includes("\\")) {
        this._showError("Folder must not contain special characters");
        return;
      }

    if (this.state.path.length == 0 && name.length > 1) {
      this._showError("Drives must be a single letter");
      return;
    }

    const fullPath = "/root/filesys/" + [...this.state.path, name].join("/");
    await this.props.filesystem.createFolder(fullPath);
    this.forceUpdate();
  }

  render() {
    const elements = getFolderElements(this.props.filesystem, this.state.path);

    return (
      <>
        <Dialog
          open={this.state.createFolder}
          onOpenChange={(open) => this.setState({ createFolder: open })}
        >
          <DialogContent className="sm:max-w-[425px]">
            <form
              onSubmit={(e) => {
                const folderName = (e.target as any).elements.name.value;
                this._onFolderCreate(folderName);
                e.preventDefault();
              }}
            >
              <DialogHeader>
                <DialogTitle>Create new folder</DialogTitle>
              </DialogHeader>
              <div className="py-4">
                <Input id="name" defaultValue="New Folder" />
              </div>
              <DialogFooter>
                <Button type="submit" className="fancy rounded-lg">
                  Create
                </Button>
              </DialogFooter>
            </form>
          </DialogContent>
        </Dialog>

        <Dialog
          open={this.state.errorText.length > 0}
          onOpenChange={(open) =>
            open ? {} : this.setState({ errorText: "" })
          }
        >
          <DialogContent className="sm:max-w-[425px]">
            <DialogHeader>
              <DialogTitle>Error</DialogTitle>
            </DialogHeader>
            <div className="py-4">{this.state.errorText}</div>
            <DialogFooter>
              <Button variant="destructive" onClick={() => this.setState({ errorText: "" })}>
                Ok
              </Button>
            </DialogFooter>
          </DialogContent>
        </Dialog>

        <Folder
          elements={elements}
          clickHandler={this._onElementSelect}
          createFolderHandler={this._showFolderCreateDialog}
        />
      </>
    );
  }
}
