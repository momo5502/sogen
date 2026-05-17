import React from "react";
import {
  Folder,
  FolderElement,
  FolderElementType,
  trimFilename,
} from "./components/folder";
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
import {
  Breadcrumb,
  BreadcrumbItem,
  BreadcrumbLink,
  BreadcrumbList,
  BreadcrumbPage,
  BreadcrumbSeparator,
} from "@/components/ui/breadcrumb";

import { HouseFill } from "react-bootstrap-icons";
import { parsePeIcon } from "./pe-icon-parser";

export interface FilesystemExplorerProps {
  filesystem: Filesystem;
  runFile: (file: string) => void;
  resetFilesys: () => void;
  path: string[];
  iconCache: Map<string, string | null>;
  linuxMode?: boolean;
}
export interface FilesystemExplorerState {
  path: string[];
  createFolder: boolean;
  resetFilesys: boolean;
  errorText: string;
  removeFile: string;
  renameFile: string;
}

function makeFullPath(path: string[], linuxMode: boolean = false) {
  const root = linuxMode ? "/root" : "/root/filesys";
  if (path.length === 0) {
    return root;
  }

  return root + "/" + path.join("/");
}

function makeFullPathAndJoin(
  path: string[],
  element: string,
  linuxMode: boolean = false,
) {
  return makeFullPath([...path, element], linuxMode);
}

function makeFullPathWithState(
  state: FilesystemExplorerState,
  element: string,
  linuxMode: boolean = false,
) {
  return makeFullPathAndJoin(state.path, element, linuxMode);
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

function makeDisplayPathWithState(
  state: FilesystemExplorerState,
  element: string,
  linuxMode: boolean = false,
) {
  if (linuxMode) {
    return "/" + makeRelativePathWithState(state, element);
  }

  return makeWindowsPathWithState(state, element);
}

function getFolderElements(
  filesystem: Filesystem,
  path: string[],
  linuxMode: boolean = false,
) {
  const fullPath = makeFullPath(path, linuxMode);
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

function selectFiles(linuxMode: boolean = false): Promise<FileList> {
  return new Promise((resolve) => {
    const fileInput = document.createElement("input");
    fileInput.type = "file";
    fileInput.accept = linuxMode ? ".elf,.so,.bin" : ".exe";

    fileInput.addEventListener("change", function (event) {
      const files = (event as any).target.files as FileList;
      resolve(files);
    });

    fileInput.click();
  });
}

function getPeIcon(
  filesystem: Filesystem,
  file: string,
  cache: Map<string, string | null>,
) {
  if (!file || !file.endsWith(".exe")) {
    return null;
  }

  const cachedValue = cache.get(file);
  if (cachedValue) {
    return cachedValue;
  }

  const data = filesystem.readFile(file);
  const icon = parsePeIcon(data);
  cache.set(file, icon);

  return icon;
}

interface BreadcrumbElement {
  node: React.ReactNode;
  targetPath: string[];
}

function isGoodPath(path: any) {
  return typeof path === "string" && path.length > 0;
}

function trimLeadingSlash(path: string) {
  if (path.startsWith("/")) {
    return path.substring(1);
  }

  return path;
}

function getFileName(file: File) {
  const fileObj = file as any;
  const properties = ["relativePath", "webkitRelativePath", "name"];

  for (let i = 0; i < properties.length; ++i) {
    const prop = properties[i];

    if (prop in fileObj) {
      const relativePath = fileObj[prop];
      if (isGoodPath(relativePath)) {
        return trimLeadingSlash(relativePath);
      }
    }
  }

  return file.name;
}

function generateBreadcrumbElements(path: string[]): BreadcrumbElement[] {
  const elements = path.map((p, index) => {
    const e: BreadcrumbElement = {
      node: p,
      targetPath: path.slice(0, index + 1),
    };

    return e;
  });
  elements.unshift({
    node: <HouseFill />,
    targetPath: [],
  });

  return elements;
}

function downloadData(
  data: Uint8Array,
  filename: string,
  mimeType: string = "application/octet-stream",
) {
  const buffer = data.buffer.slice(
    data.byteOffset,
    data.byteOffset + data.byteLength,
  ) as ArrayBuffer;
  const blob = new Blob([buffer], { type: mimeType });
  const url = URL.createObjectURL(blob);

  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  link.click();

  URL.revokeObjectURL(url);
}

export class FilesystemExplorer extends React.Component<
  FilesystemExplorerProps,
  FilesystemExplorerState
> {
  constructor(props: FilesystemExplorerProps) {
    super(props);

    this._onAddFiles = this._onAddFiles.bind(this);
    this._uploadFiles = this._uploadFiles.bind(this);
    this._onElementSelect = this._onElementSelect.bind(this);

    this.state = {
      path: this.props.path,
      createFolder: false,
      resetFilesys: false,
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
      if (this.props.linuxMode) {
        const file = "./root/" + makeRelativePathWithState(this.state, element.name);
        this.props.runFile(file);
      } else if (element.name.endsWith(".exe")) {
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
    if (!this.props.linuxMode) {
      newFile = newFile.toLowerCase();
    }

    if (newFile == file) {
      this.setState({ renameFile: "" });
      return;
    }

    if (!this._validateName(newFile)) {
      return;
    }

    const oldPath = makeFullPathWithState(this.state, file, this.props.linuxMode);
    const newPath = makeFullPathWithState(
      this.state,
      newFile,
      this.props.linuxMode,
    );

    this.setState({ renameFile: "" });

    this._removeFromCache(file);
    this._removeFromCache(newFile);

    await this.props.filesystem.rename(oldPath, newPath);
    this.forceUpdate();
  }

  async _onAddFiles() {
    const files = await selectFiles(this.props.linuxMode);
    await this._uploadFiles(files);
  }

  _validateName(name: string) {
    if (name.length == 0) {
      return false;
    }

    if (name.includes("/") || name.includes("\\")) {
      this._showError("Folder must not contain special characters");
      return false;
    }

    if (!this.props.linuxMode && this.state.path.length == 0 && name.length > 1) {
      this._showError("Drives must be a single letter");
      return false;
    }

    return true;
  }

  async _onFolderCreate(name: string) {
    if (!this.props.linuxMode) {
      name = name.toLowerCase();
    }

    if (!this._validateName(name)) {
      return;
    }

    this.setState({ createFolder: false });

    const fullPath = makeFullPathWithState(
      this.state,
      name,
      this.props.linuxMode,
    );
    await this.props.filesystem.createFolder(fullPath);
    this.forceUpdate();
  }

  async _uploadFiles(files: FileList | File[]) {
    if (files.length == 0) {
      return;
    }

    if (!this.props.linuxMode && this.state.path.length == 0) {
      this._showError("Files must be within a drive");
      return;
    }

    const fileData = (await readFiles(files)).map((f) => {
      const name = getFileName(f.file);
      const normalizedName = this.props.linuxMode ? name : name.toLowerCase();
      return {
        name: makeFullPathWithState(
          this.state,
          normalizedName,
          this.props.linuxMode,
        ),
        data: f.data,
      };
    });

    fileData.forEach((d) => {
      this._removeFromCache(d.name);
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
              <DialogTitle>
                Rename {trimFilename(this.state.renameFile)}
              </DialogTitle>
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
              className="fancy rounded-lg"
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
            <DialogTitle>
              Delete {trimFilename(this.state.removeFile)}?
            </DialogTitle>
            <DialogDescription className="hidden">
              Delete {this.state.removeFile}
            </DialogDescription>
          </DialogHeader>
          <div className="py-4">
            Are you sure you want to delete{" "}
            <b className="break-all">
              {makeDisplayPathWithState(
                this.state,
                this.state.removeFile,
                this.props.linuxMode,
              )}
            </b>
          </div>
          <DialogFooter>
            <Button
              variant="destructive"
              className="fancy rounded-lg"
              onClick={() => {
                const file = makeFullPathWithState(
                  this.state,
                  this.state.removeFile,
                  this.props.linuxMode,
                );
                this.setState({ removeFile: "" });
                this._removeFromCache(file);
                this.props.filesystem
                  .unlink(file)
                  .then(() => this.forceUpdate());
              }}
            >
              Delete
            </Button>
            <Button
              variant="secondary"
              className="fancy rounded-lg"
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

  _renderResetDialog() {
    return (
      <Dialog
        open={this.state.resetFilesys}
        onOpenChange={(open) => this.setState({ resetFilesys: open })}
      >
        <DialogContent className="sm:max-w-[425px]">
          <DialogHeader>
            <DialogTitle>Reset filesystem</DialogTitle>
            <DialogDescription className="hidden">
              Reset filesystem
            </DialogDescription>
          </DialogHeader>
          <div className="py-4">
            Are you sure you want to reset the filesystem?
          </div>
          <DialogFooter>
            <Button
              variant="destructive"
              className="fancy rounded-lg"
              onClick={() => {
                this.setState({ resetFilesys: false });
                this.props.iconCache.clear();
                this.props.resetFilesys();
              }}
            >
              Reset
            </Button>
            <Button
              variant="secondary"
              className="fancy rounded-lg"
              onClick={() => {
                this.setState({ resetFilesys: false });
              }}
            >
              Cancel
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    );
  }

  _renderBreadcrumbElements() {
    const elements = generateBreadcrumbElements(this.state.path);

    return elements.map((e, index) => {
      if (index == this.state.path.length) {
        return (
          <BreadcrumbItem key={`breadcrumb-item-${index}`}>
            <BreadcrumbPage key={`breadcrumb-page-${index}`}>
              {e.node}
            </BreadcrumbPage>
          </BreadcrumbItem>
        );
      }

      const navigate = () => this.setState({ path: e.targetPath });
      return [
        <BreadcrumbItem key={`breadcrumb-item-${index}`}>
          <BreadcrumbLink key={`breadcrumb-link-${index}`} onClick={navigate}>
            {e.node}
          </BreadcrumbLink>
        </BreadcrumbItem>,
        <BreadcrumbSeparator key={`breadcrumb-separator-${index}`} />,
      ];
    });
  }

  _renderBreadCrumb() {
    return (
      <Breadcrumb>
        <BreadcrumbList>{this._renderBreadcrumbElements()}</BreadcrumbList>
      </Breadcrumb>
    );
  }

  _removeFromCache(file: string) {
    this.props.iconCache.delete(file);
  }

  _downloadFile(file: string) {
    const fullPath = makeFullPathWithState(
      this.state,
      file,
      this.props.linuxMode,
    );
    const data = this.props.filesystem.readFile(fullPath);
    downloadData(data, file);
  }

  render() {
    const elements = getFolderElements(
      this.props.filesystem,
      this.state.path,
      this.props.linuxMode,
    );

    return (
      <>
        {this._renderCreateFolderDialog()}
        {this._renderRenameDialog()}
        {this._renderErrorDialog()}
        {this._renderRemoveDialog()}
        {this._renderResetDialog()}

        <div className="flex flex-row w-full items-center gap-3">
          <div className="whitespace-nowrap">{this._renderBreadCrumb()}</div>
          <div className="flex-1 text-right">
            <Button
              onClick={() => this.setState({ resetFilesys: true })}
              variant="destructive"
              size="sm"
              className="fancy rounded-lg"
            >
              Reset
            </Button>
          </div>
        </div>

        <Dropzone onDrop={this._uploadFiles} noClick={true}>
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
                downloadElementHandler={(e) => this._downloadFile(e.name)}
                addFilesHandler={this._onAddFiles}
                iconReader={(e) =>
                  getPeIcon(
                    this.props.filesystem,
                    makeFullPathWithState(
                      this.state,
                      e.name,
                      this.props.linuxMode,
                    ),
                    this.props.iconCache,
                  )
                }
              />
            </div>
          )}
        </Dropzone>
      </>
    );
  }
}
