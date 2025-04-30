import React from "react";
import { Folder, FolderElement, FolderElementType } from "./components/folder";
import { Filesystem } from "./filesystem";

import {
  Dialog,
  DialogClose,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Button } from "./components/ui/button";
import { Input } from "./components/ui/input";
import { DialogDescription } from "@radix-ui/react-dialog";

import Dropzone from "react-dropzone";

export interface FilesystemExplorerProps {
  filesystem: Filesystem;
  runFile: (file: string) => void;
}
export interface FilesystemExplorerState {
  path: string[];
  createFolder: boolean;
  errorText: string;
  removeFile: string;
  renameFile: string;
}

function makeFullPath(path: string[]) {
  return "/root/filesys/" + path.join("/");
}

function makeFullPathAndJoin(path: string[], element: string) {
  return makeFullPath([...path, element]);
}

function makeFullPathWithState(
  state: FilesystemExplorerState,
  element: string,
) {
  return makeFullPathAndJoin(state.path, element);
}

function relativePathToWindowsPath(fullPath: string) {
  if (fullPath.length == 0) {
    return fullPath;
  }

  const drive = fullPath.substring(0, 1);
  const rest = fullPath.substring(1);

  return `${drive}:${rest}`;
}

function makeRelativePathWithState(
  state: FilesystemExplorerState,
  element: string,
) {
  return [...state.path, element].join("/");
}

function makeWindowsPathWithState(
  state: FilesystemExplorerState,
  element: string,
) {
  const fullPath = makeRelativePathWithState(state, element);
  return relativePathToWindowsPath(fullPath);
}

function getFolderElements(filesystem: Filesystem, path: string[]) {
  const fullPath = makeFullPath(path);
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

interface FileWithData {
  file: File;
  data: ArrayBuffer;
}

function readFile(file: File): Promise<FileWithData> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      if (reader.readyState === FileReader.DONE) {
        resolve({
          file,
          data: reader.result as ArrayBuffer,
        });
      }
    };
    reader.onerror = reject;
    reader.readAsArrayBuffer(file);
  });
}

async function readFiles(files: FileList | File[]): Promise<FileWithData[]> {
  const promises = [];

  for (let i = 0; i < files.length; i++) {
    promises.push(readFile(files[i]));
  }

  return Promise.all(promises);
}

export class FilesystemExplorer extends React.Component<
  FilesystemExplorerProps,
  FilesystemExplorerState
> {
  constructor(props: FilesystemExplorerProps) {
    super(props);

    this._onFileDrop = this._onFileDrop.bind(this);
    this._onElementSelect = this._onElementSelect.bind(this);

    this.state = {
      path: [],
      createFolder: false,
      errorText: "",
      removeFile: "",
      renameFile: "",
    };
  }

  _showError(errorText: string) {
    this.setState({ errorText });
  }

  _onElementSelect(element: FolderElement) {
    if (element.type != FolderElementType.Folder) {
      if (element.name.endsWith(".exe")) {
        const file = makeWindowsPathWithState(this.state, element.name);
        this.props.runFile(file);
      }

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

  async _onFileRename(file: string, newFile: string) {
    const oldPath = makeFullPathWithState(this.state, file);
    const newPath = makeFullPathWithState(this.state, newFile);

    this.setState({ renameFile: "" });

    await this.props.filesystem.rename(oldPath, newPath);
    this.forceUpdate();
  }

  async _onFolderCreate(name: string) {
    this.setState({ createFolder: false });

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

    const fullPath = makeFullPathWithState(this.state, name);
    await this.props.filesystem.createFolder(fullPath);
    this.forceUpdate();
  }

  async _onFileDrop(files: FileList | File[]) {
    const fileData = (await readFiles(files)).map((f) => {
      return {
        name: makeFullPathWithState(this.state, f.file.name.toLowerCase()),
        data: f.data,
      };
    });

    await this.props.filesystem.storeFiles(fileData);
    this.forceUpdate();
  }

  _renderCreateFolderDialog() {
    return (
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
              <DialogDescription className="hidden">
                Create new folder
              </DialogDescription>
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
    );
  }

  _renderRenameDialog() {
    return (
      <Dialog
        open={this.state.renameFile.length > 0}
        onOpenChange={(open) => (open ? {} : this.setState({ renameFile: "" }))}
      >
        <DialogContent className="sm:max-w-[425px]">
          <form
            onSubmit={(e) => {
              const newName = (e.target as any).elements.name.value;
              this._onFileRename(this.state.renameFile, newName);
              e.preventDefault();
            }}
          >
            <DialogHeader>
              <DialogTitle>Rename {this.state.renameFile}</DialogTitle>
              <DialogDescription className="hidden">
                Rename {this.state.renameFile}
              </DialogDescription>
            </DialogHeader>
            <div className="py-4">
              <Input id="name" defaultValue={this.state.renameFile} />
            </div>
            <DialogFooter>
              <Button type="submit" className="fancy rounded-lg">
                Rename
              </Button>
              <DialogClose asChild>
                <Button variant="secondary" className="fancy rounded-lg">
                  Cancel
                </Button>
              </DialogClose>
            </DialogFooter>
          </form>
        </DialogContent>
      </Dialog>
    );
  }

  _renderErrorDialog() {
    return (
      <Dialog
        open={this.state.errorText.length > 0}
        onOpenChange={(open) => (open ? {} : this.setState({ errorText: "" }))}
      >
        <DialogContent className="sm:max-w-[425px]">
          <DialogHeader>
            <DialogTitle>Error</DialogTitle>
            <DialogDescription className="hidden">
              Error: {this.state.errorText}
            </DialogDescription>
          </DialogHeader>
          <div className="py-4">{this.state.errorText}</div>
          <DialogFooter>
            <Button
              variant="destructive"
              onClick={() => this.setState({ errorText: "" })}
            >
              Ok
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    );
  }

  _renderRemoveDialog() {
    return (
      <Dialog
        open={this.state.removeFile.length > 0}
        onOpenChange={(open) => (open ? {} : this.setState({ removeFile: "" }))}
      >
        <DialogContent className="sm:max-w-[425px]">
          <DialogHeader>
            <DialogTitle>Delete {this.state.removeFile}?</DialogTitle>
            <DialogDescription className="hidden">
              Delete {this.state.removeFile}
            </DialogDescription>
          </DialogHeader>
          <div className="py-4">
            Are you sure you want to delete{" "}
            <b>
              {makeRelativePathWithState(this.state, this.state.removeFile)}
            </b>
          </div>
          <DialogFooter>
            <Button
              variant="destructive"
              onClick={() => {
                const file = makeFullPathWithState(
                  this.state,
                  this.state.removeFile,
                );
                this.setState({ removeFile: "" });
                this.props.filesystem
                  .unlink(file)
                  .then(() => this.forceUpdate());
              }}
            >
              Ok
            </Button>
            <Button
              variant="secondary"
              onClick={() => {
                this.setState({ removeFile: "" });
              }}
            >
              Cancel
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    );
  }

  render() {
    const elements = getFolderElements(this.props.filesystem, this.state.path);

    return (
      <>
        {this._renderCreateFolderDialog()}
        {this._renderRenameDialog()}
        {this._renderErrorDialog()}
        {this._renderRemoveDialog()}

        <Dropzone onDrop={this._onFileDrop} noClick={true}>
          {({ getRootProps, getInputProps }) => (
            <div {...getRootProps()}>
              <input {...getInputProps()} />
              <Folder
                elements={elements}
                clickHandler={this._onElementSelect}
                createFolderHandler={() =>
                  this.setState({ createFolder: true })
                }
                removeElementHandler={(e) =>
                  this.setState({ removeFile: e.name })
                }
                renameElementHandler={(e) =>
                  this.setState({ renameFile: e.name })
                }
              />
            </div>
          )}
        </Dropzone>
      </>
    );
  }
}
