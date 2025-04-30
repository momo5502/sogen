import * as PE from "pe-library";

function patchExeFile(exe: PE.NtExecutable) {
  // The PE library doesn't support parsing resources if other sections follow
  // This might make sense, as the library will have issues rewriting the PE file.
  // As we only care about parsing though, just kill the other sections.
  const rsrc = exe.getSectionByEntry(PE.Format.ImageDirectoryEntry.Resource);
  const orig = exe.getAllSections.bind(exe);
  exe.getAllSections = function () {
    let x = { skip: false };
    return orig().filter((s) => {
      if (x.skip) {
        return false;
      }
      if (s == rsrc) {
        x.skip = true;
      }

      return true;
    });
  };
}

function arrayBufferToBase64(bytes: Uint8Array) {
  let binary = "";
  const len = bytes.byteLength;
  for (let i = 0; i < len; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}

function isPng(buffer: Uint8Array) {
  if (buffer.length < 4) {
    return false;
  }

  return buffer[1] === 80 && buffer[2] === 78 && buffer[3] === 71;
}

function generateDataURL(arrayBuffer: Uint8Array, contentType: string) {
  const base64 = arrayBufferToBase64(arrayBuffer);
  return `data:${contentType};base64,${base64}`;
}

interface IconEntry {
  width: number;
  height: number;
  colorCount: number;
  reserved: number;
  planes: number;
  bitCount: number;
  bytesInRes: number;
  id: number;
}

interface IconGroup {
  reserved: number;
  type: number;
  icons: IconEntry[];
}

function writeUint8(buffer: Uint8Array, offset: number, value: number) {
  buffer[offset] = value;
}

function writeUint16(buffer: Uint8Array, offset: number, value: number) {
  writeUint8(buffer, offset + 0, value & 0xff);
  writeUint8(buffer, offset + 1, (value >> 8) & 0xff);
}

function writeUint32(buffer: Uint8Array, offset: number, value: number) {
  writeUint16(buffer, offset + 0, value & 0xffff);
  writeUint16(buffer, offset + 2, (value >> 16) & 0xffff);
}

function readUInt8(buffer: Uint8Array, offset: number) {
  return buffer[offset];
}

function readUInt16(buffer: Uint8Array, offset: number) {
  return readUInt8(buffer, offset) | (readUInt8(buffer, offset + 1) << 8);
}

function readUInt32(buffer: Uint8Array, offset: number) {
  return readUInt16(buffer, offset) | (readUInt16(buffer, offset + 2) << 16);
}

function parseIconGroup(buffer: Uint8Array): IconGroup {
  const reserved = readUInt16(buffer, 0);
  const type = readUInt16(buffer, 2);
  const count = readUInt16(buffer, 4);

  const icons: IconEntry[] = [];

  for (let i = 0; i < count; ++i) {
    const start = 6 + i * 14;
    const width = readUInt8(buffer, start + 0);
    const height = readUInt8(buffer, start + 1);
    const colorCount = readUInt8(buffer, start + 2);
    const reserved2 = readUInt8(buffer, start + 3);
    const planes = readUInt16(buffer, start + 4);
    const bitCount = readUInt16(buffer, start + 6);
    const bytesInRes = readUInt32(buffer, start + 8);
    const id = readUInt16(buffer, start + 12);

    icons.push({
      width,
      height,
      colorCount,
      reserved: reserved2,
      planes,
      bitCount,
      bytesInRes,
      id,
    });
  }

  return {
    reserved,
    type,
    icons,
  };
}

function mergeArrayBuffers(
  buffer1: ArrayBuffer,
  buffer2: ArrayBuffer,
): ArrayBuffer {
  const mergedBuffer = new ArrayBuffer(buffer1.byteLength + buffer2.byteLength);

  const view1 = new Uint8Array(buffer1);
  const view2 = new Uint8Array(buffer2);
  const mergedView = new Uint8Array(mergedBuffer);

  mergedView.set(view1, 0);
  mergedView.set(view2, buffer1.byteLength);

  return mergedBuffer;
}

function generateIcoHeader(icon: IconEntry) {
  const headerSize = 0x16;
  const header = new Uint8Array(headerSize);
  writeUint8(header, 2, 1); // Image type -> ico
  writeUint8(header, 4, 1); // Image count

  const start = 6;

  writeUint8(header, start + 0, icon.width);
  writeUint8(header, start + 1, icon.height);
  writeUint8(header, start + 2, icon.colorCount);

  writeUint16(header, start + 4, icon.planes);
  writeUint16(header, start + 6, icon.bitCount);
  writeUint32(header, start + 8, icon.bytesInRes);
  writeUint32(header, start + 12, headerSize);

  return header;
}

function isMaxResIcon(icon: IconEntry) {
  return icon.width == 0 && icon.height == 0;
}

function getBiggestIcon(group: IconGroup) {
  if (group.icons.length == 0) {
    return null;
  }

  var biggest = group.icons[0];
  if (isMaxResIcon(biggest)) {
    return biggest;
  }

  for (let i = 1; i < group.icons.length; ++i) {
    let current = group.icons[i];
    if (isMaxResIcon(current)) {
      return current;
    }

    if (current.width * current.height > biggest.width * biggest.height) {
      biggest = current;
    }
  }

  return biggest;
}

function getPeResources(data: Uint8Array) {
  const exe = PE.NtExecutable.from(data, { ignoreCert: true });
  patchExeFile(exe);
  return PE.NtExecutableResource.from(exe, true);
}

function getIconDataUrl(iconEntry: IconEntry, iconData: ArrayBuffer) {
  let contentType = "image/png";

  if (!isPng(new Uint8Array(iconData))) {
    contentType = "image/x-icon";

    const header = generateIcoHeader(iconEntry);
    iconData = mergeArrayBuffers(header, iconData);
  }

  return generateDataURL(new Uint8Array(iconData), contentType);
}

export function parsePeIcon(data: Uint8Array) {
  const res = getPeResources(data);
  const icons = res.entries.filter((e) => e.type == 3);
  const iconGroups = res.entries.filter((e) => e.type == 14);

  if (iconGroups.length == 0 || icons.length == 0) {
    return null;
  }

  const groupData = new Uint8Array(iconGroups[0].bin);
  const group = parseIconGroup(groupData);
  const iconEntry = getBiggestIcon(group);

  if (!iconEntry) {
    return null;
  }

  const icon = icons.find((i) => i.id == iconEntry.id);
  if (!icon) {
    return null;
  }

  return getIconDataUrl(iconEntry, icon.bin);
}
