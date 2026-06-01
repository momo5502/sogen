export interface SogenUiHostMessage {
  type: "sogen_ui";
  command: string;
  hwnd?: number;
  parent?: number;
  owner?: number;
  rect?: { left: number; top: number; right: number; bottom: number };
  class_name?: string;
  title?: string;
  style?: number;
  ex_style?: number;
  control_id?: number;
  visible?: boolean;
  enabled?: boolean;
  top_level?: boolean;
  value?: boolean;
  width?: number;
  height?: number;
  stride?: number;
  format?: number;
  pixels?: Uint8Array | ArrayBuffer;
}

interface HostWindowState {
  hwnd: number;
  parent: number;
  owner: number;
  rect: { left: number; top: number; right: number; bottom: number };
  className: string;
  title: string;
  visible: boolean;
  enabled: boolean;
  topLevel: boolean;
  imageData: ImageData | null;
  surfaceCanvas: HTMLCanvasElement | null;
}

const PLAYGROUND_BACKGROUND = "#18181b";
const WINDOW_BACKGROUND = "#f5f5f5";
const WINDOW_DISABLED_BACKGROUND = "#e5e7eb";
const TITLE_BACKGROUND = "#27272a";
const WINDOW_BORDER = "#71717a";
const CHILD_BORDER = "#a1a1aa";

interface DragState {
  hwnd: number;
  offsetX: number;
  offsetY: number;
}

interface AttachSogenUiHostOptions {
  onWindowCountChanged?: (count: number) => void;
}

const WM_KEYDOWN = 0x0100;
const WM_KEYUP = 0x0101;
const WM_CHAR = 0x0102;
const WM_MOUSEMOVE = 0x0200;
const WM_LBUTTONDOWN = 0x0201;
const WM_LBUTTONUP = 0x0202;
const WM_RBUTTONDOWN = 0x0204;
const WM_RBUTTONUP = 0x0205;
const TITLE_BAR_HEIGHT = 24;
const TOP_LEVEL_CLIENT_OFFSET_Y = TITLE_BAR_HEIGHT + 8;

function cloneRect(rect?: { left: number; top: number; right: number; bottom: number }) {
  return rect ?? { left: 0, top: 0, right: 0, bottom: 0 };
}

function convertSurfacePixels(
  width: number,
  height: number,
  format: number,
  pixels: Uint8Array,
) {
  if (format !== 0) {
    return Uint8ClampedArray.from(pixels.subarray(0, width * height * 4));
  }

  const rgba = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < width * height; ++i) {
    const src = i * 4;
    rgba[src + 0] = pixels[src + 2] ?? 0;
    rgba[src + 1] = pixels[src + 1] ?? 0;
    rgba[src + 2] = pixels[src + 0] ?? 0;
    rgba[src + 3] = pixels[src + 3] ?? 255;
  }

  return rgba;
}

export function attachSogenUiHost(
  worker: Worker,
  canvas: HTMLCanvasElement,
  options: AttachSogenUiHostOptions = {},
) {
  const context = canvas.getContext("2d");
  if (!context) {
    throw new Error("Failed to create 2D canvas context");
  }

  const context2d = context;

  const windows = new Map<number, HostWindowState>();
  let focusedWindow = 0;
  let dragState: DragState | null = null;

  function notifyWindowCount() {
    options.onWindowCountChanged?.(windows.size);
  }

  function getWindow(hwnd: number) {
    let window = windows.get(hwnd);
    if (!window) {
      window = {
        hwnd,
        parent: 0,
        owner: 0,
        rect: cloneRect(),
        className: "",
        title: "",
        visible: false,
        enabled: true,
        topLevel: true,
        imageData: null,
        surfaceCanvas: null,
      };
      windows.set(hwnd, window);
      notifyWindowCount();
    }

    return window;
  }

  function packPoint(x: number, y: number) {
    return ((y & 0xffff) << 16) | (x & 0xffff);
  }

  function unpackPixels(message: SogenUiHostMessage) {
    if (!message.pixels || !message.width || !message.height) {
      return null;
    }

    const pixels =
      message.pixels instanceof Uint8Array
        ? message.pixels
        : new Uint8Array(message.pixels);
    const rgba = convertSurfacePixels(
      message.width,
      message.height,
      message.format ?? 0,
      pixels,
    );
    return new ImageData(rgba, message.width, message.height);
  }

  function updateSurfaceCanvas(window: HostWindowState, imageData: ImageData | null) {
    window.imageData = imageData;
    if (!imageData) {
      window.surfaceCanvas = null;
      return;
    }

    const surfaceCanvas = window.surfaceCanvas ?? document.createElement("canvas");
    surfaceCanvas.width = imageData.width;
    surfaceCanvas.height = imageData.height;
    const surfaceContext = surfaceCanvas.getContext("2d");
    if (!surfaceContext) {
      window.surfaceCanvas = null;
      return;
    }

    surfaceContext.putImageData(imageData, 0, 0);
    window.surfaceCanvas = surfaceCanvas;
  }

  function resizeCanvas() {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(rect.width * dpr));
    const height = Math.max(1, Math.round(rect.height * dpr));

    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }

    context2d.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function bringToFront(hwnd: number) {
    const window = windows.get(hwnd);
    if (!window) {
      return;
    }

    windows.delete(hwnd);
    windows.set(hwnd, window);
  }

  function drawChildren(parent: HostWindowState) {
    const clientOffsetY = parent.topLevel ? TOP_LEVEL_CLIENT_OFFSET_Y : 0;

    for (const child of windows.values()) {
      if (!child.visible || child.parent !== parent.hwnd) {
        continue;
      }

      const width = Math.max(1, child.rect.right - child.rect.left);
      const height = Math.max(1, child.rect.bottom - child.rect.top);
      const className = child.className.toLowerCase();

      context2d.save();
      context2d.translate(child.rect.left, child.rect.top + clientOffsetY);

      if (className === "button") {
        context2d.fillStyle = child.enabled ? "#e5e7eb" : "#d4d4d8";
        context2d.fillRect(0, 0, width, height);
        context2d.strokeStyle = CHILD_BORDER;
        context2d.lineWidth = 1;
        context2d.strokeRect(0.5, 0.5, width - 1, height - 1);
        context2d.fillStyle = "#18181b";
        context2d.font = "12px Inter, sans-serif";
        context2d.textAlign = "center";
        context2d.textBaseline = "middle";
        context2d.fillText(child.title || child.className || "Button", width / 2, height / 2);
      } else if (className === "static") {
        context2d.fillStyle = "#18181b";
        context2d.font = "12px Inter, sans-serif";
        context2d.textAlign = "left";
        context2d.textBaseline = "top";
        context2d.fillText(child.title || "", 0, 0);
      } else {
        context2d.fillStyle = child.enabled ? "#fafafa" : "#e4e4e7";
        context2d.fillRect(0, 0, width, height);
        context2d.strokeStyle = CHILD_BORDER;
        context2d.lineWidth = 1;
        context2d.strokeRect(0.5, 0.5, width - 1, height - 1);
        context2d.fillStyle = "#52525b";
        context2d.font = "11px Inter, sans-serif";
        context2d.textAlign = "left";
        context2d.textBaseline = "top";
        context2d.fillText(child.title || child.className || `0x${child.hwnd.toString(16)}`, 6, 6);
      }

      if (child.imageData) {
        context2d.putImageData(child.imageData, 0, 0);
      }

      context2d.restore();
    }
  }

  function drawWindow(window: HostWindowState) {
    if (!window.visible) {
      return;
    }

    const width = Math.max(1, window.rect.right - window.rect.left);
    const height = Math.max(1, window.rect.bottom - window.rect.top);

    context2d.save();
    context2d.translate(window.rect.left, window.rect.top);

    context2d.fillStyle = window.enabled ? WINDOW_BACKGROUND : WINDOW_DISABLED_BACKGROUND;
    context2d.fillRect(0, 0, width, height);

    context2d.strokeStyle = window.topLevel ? WINDOW_BORDER : CHILD_BORDER;
    context2d.lineWidth = 1;
    context2d.strokeRect(0.5, 0.5, width - 1, height - 1);

    context2d.fillStyle = TITLE_BACKGROUND;
    context2d.fillRect(1, 1, Math.max(0, width - 2), Math.min(TITLE_BAR_HEIGHT, Math.max(0, height - 2)));

    context2d.fillStyle = "#e2e8f0";
    context2d.font = "12px Inter, sans-serif";
    context2d.textBaseline = "middle";
    context2d.fillText(window.title || window.className || `0x${window.hwnd.toString(16)}`, 8, 12);

    if (window.surfaceCanvas) {
      context2d.drawImage(window.surfaceCanvas, 0, 0);
    }

    drawChildren(window);

    if (!window.imageData && !Array.from(windows.values()).some((child) => child.parent === window.hwnd && child.visible)) {
      context2d.fillStyle = "#71717a";
      context2d.font = "11px Inter, sans-serif";
      context2d.fillText(window.className || "window", 8, Math.min(height - 12, 40));
    }

    context2d.restore();
  }

  function composite() {
    resizeCanvas();

    const rect = canvas.getBoundingClientRect();
    context2d.clearRect(0, 0, rect.width, rect.height);
    context2d.fillStyle = PLAYGROUND_BACKGROUND;
    context2d.fillRect(0, 0, rect.width, rect.height);

    for (const window of windows.values()) {
      if (window.parent === 0) {
        drawWindow(window);
      }
    }
  }

  function hitTest(clientX: number, clientY: number) {
    let hit: HostWindowState | null = null;
    for (const window of windows.values()) {
      if (!window.visible || window.parent !== 0) {
        continue;
      }

      if (
        clientX >= window.rect.left &&
        clientX < window.rect.right &&
        clientY >= window.rect.top &&
        clientY < window.rect.bottom
      ) {
        hit = window;
      }
    }

    return hit;
  }

  function sendUiEvent(hwnd: number, message: number, wParam: number, lParam: number) {
    worker.postMessage({
      type: "sogen_ui_event",
      window: hwnd >>> 0,
      message: message >>> 0,
      wParam: wParam >>> 0,
      lParam: lParam >>> 0,
    });
  }

  function toCanvasPoint(event: MouseEvent) {
    const rect = canvas.getBoundingClientRect();
    return {
      x: Math.floor(event.clientX - rect.left),
      y: Math.floor(event.clientY - rect.top),
    };
  }

  function onWorkerMessage(event: MessageEvent) {
    const message = event.data as SogenUiHostMessage;
    if (!message || message.type !== "sogen_ui") {
      return;
    }

    if (message.command === "host_ready") {
      return;
    }

    const hwnd = message.hwnd ?? 0;
    if (!hwnd) {
      return;
    }

    switch (message.command) {
      case "create_window": {
        const window = getWindow(hwnd);
        window.parent = message.parent ?? 0;
        window.owner = message.owner ?? 0;
        window.rect = cloneRect(message.rect);
        window.className = message.class_name ?? "";
        window.title = message.title ?? "";
        window.visible = !!message.visible;
        window.enabled = message.enabled ?? true;
        window.topLevel = message.top_level ?? true;
        break;
      }
      case "destroy_window":
        windows.delete(hwnd);
        notifyWindowCount();
        if (focusedWindow === hwnd) {
          focusedWindow = 0;
        }
        break;
      case "set_rect":
        getWindow(hwnd).rect = cloneRect(message.rect);
        break;
      case "set_visible":
        getWindow(hwnd).visible = !!message.value;
        break;
      case "set_enabled":
        getWindow(hwnd).enabled = !!message.value;
        break;
      case "set_title":
        getWindow(hwnd).title = message.title ?? "";
        break;
      case "present_surface":
        updateSurfaceCanvas(getWindow(hwnd), unpackPixels(message));
        break;
      default:
        break;
    }

    composite();
  }

  function onMouse(message: number, event: MouseEvent) {
    const point = toCanvasPoint(event);

    if (message === WM_MOUSEMOVE && dragState) {
      const target = windows.get(dragState.hwnd);
      if (!target) {
        dragState = null;
        return;
      }

      const width = target.rect.right - target.rect.left;
      const height = target.rect.bottom - target.rect.top;
      target.rect = {
        left: point.x - dragState.offsetX,
        top: point.y - dragState.offsetY,
        right: point.x - dragState.offsetX + width,
        bottom: point.y - dragState.offsetY + height,
      };
      composite();
      return;
    }

    const window = hitTest(point.x, point.y);
    const target = window ?? (focusedWindow ? windows.get(focusedWindow) ?? null : null);
    if (!target) {
      return;
    }

    focusedWindow = target.hwnd;
    bringToFront(target.hwnd);

    const localX = point.x - target.rect.left;
    const localY = point.y - target.rect.top;

    if (message === WM_LBUTTONDOWN && target.topLevel && localY >= 0 && localY < TITLE_BAR_HEIGHT) {
      dragState = {
        hwnd: target.hwnd,
        offsetX: localX,
        offsetY: localY,
      };
      composite();
      return;
    }

    if ((message === WM_LBUTTONUP || message === WM_RBUTTONUP) && dragState) {
      dragState = null;
      composite();
      return;
    }

    sendUiEvent(target.hwnd, message, 0, packPoint(localX, localY));
  }

  function onKey(message: number, event: KeyboardEvent) {
    if (!focusedWindow) {
      return;
    }

    const key = event.key.length === 1 ? event.key.toUpperCase().charCodeAt(0) : 0;
    sendUiEvent(focusedWindow, message, key || event.keyCode || 0, 0);
  }

  canvas.tabIndex = 0;
  canvas.addEventListener("mousedown", (event) => {
    canvas.focus();
    onMouse(event.button === 2 ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, event);
  });
  canvas.addEventListener("mouseup", (event) => {
    onMouse(event.button === 2 ? WM_RBUTTONUP : WM_LBUTTONUP, event);
  });
  canvas.addEventListener("mousemove", (event) => {
    onMouse(WM_MOUSEMOVE, event);
  });
  canvas.addEventListener("keydown", (event) => {
    onKey(WM_KEYDOWN, event);
  });
  canvas.addEventListener("keyup", (event) => {
    onKey(WM_KEYUP, event);
  });
  canvas.addEventListener("keypress", (event) => {
    if (!focusedWindow || event.key.length !== 1) {
      return;
    }

    sendUiEvent(focusedWindow, WM_CHAR, event.key.charCodeAt(0), 0);
  });
  canvas.addEventListener("contextmenu", (event) => event.preventDefault());

  const resizeObserver = new ResizeObserver(() => {
    composite();
  });
  resizeObserver.observe(canvas);

  worker.addEventListener("message", onWorkerMessage);
  composite();

  return {
    dispose() {
      resizeObserver.disconnect();
      worker.removeEventListener("message", onWorkerMessage);
      windows.clear();
      notifyWindowCount();
      composite();
    },
  };
}
