import React from "react";
import { Folder, FolderElement, FolderElementType } from "./components/folder";
import { Filesystem } from "./filesystem";

export interface FilesystemExplorerProps {
  filesystem: Filesystem;
}
export interface FilesystemExplorerState {
  path: string[];
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

    this._onElementSelect = this._onElementSelect.bind(this);

    this.state = {
      path: [],
    };
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

  render() {
    const elements = getFolderElements(this.props.filesystem, this.state.path);

    return <Folder elements={elements} clickHandler={this._onElementSelect} />;
  }
}
