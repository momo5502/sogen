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
  client_insets?: { left: number; top: number; right: number; bottom: number };
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
  controlId: number;
  clientInsets: { left: number; top: number; right: number; bottom: number };
  visible: boolean;
  enabled: boolean;
  topLevel: boolean;
  imageData: ImageData | null;
  surfaceCanvas: HTMLCanvasElement | null;
}

const PLAYGROUND_BACKGROUND = "#18181b";

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

function cloneRect(rect?: {
  left: number;
  top: number;
  right: number;
  bottom: number;
}) {
  return rect ?? { left: 0, top: 0, right: 0, bottom: 0 };
}

function convertSurfacePixels(
  width: number,
  height: number,
  stride: number,
  format: number,
  pixels: Uint8Array,
) {
  const rgba = new Uint8ClampedArray(width * height * 4);

  for (let y = 0; y < height; ++y) {
    const sourceBase = y * stride;
    const destBase = y * width * 4;

    for (let x = 0; x < width; ++x) {
      const sourceIndex = sourceBase + x * 4;
      const destIndex = destBase + x * 4;

      if (format === 0) {
        rgba[destIndex] = pixels[sourceIndex + 2] ?? 0;
        rgba[destIndex + 1] = pixels[sourceIndex + 1] ?? 0;
        rgba[destIndex + 2] = pixels[sourceIndex] ?? 0;
        rgba[destIndex + 3] = pixels[sourceIndex + 3] ?? 255;
      } else {
        rgba[destIndex] = pixels[sourceIndex] ?? 0;
        rgba[destIndex + 1] = pixels[sourceIndex + 1] ?? 0;
        rgba[destIndex + 2] = pixels[sourceIndex + 2] ?? 0;
        rgba[destIndex + 3] = pixels[sourceIndex + 3] ?? 255;
      }
    }
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
        controlId: 0,
        clientInsets: { left: 0, top: 0, right: 0, bottom: 0 },
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
    if (
      !message.pixels ||
      !message.width ||
      !message.height ||
      !message.stride
    ) {
      return null;
    }

    const pixels =
      message.pixels instanceof Uint8Array
        ? message.pixels
        : new Uint8Array(message.pixels);
    const rgba = convertSurfacePixels(
      message.width,
      message.height,
      message.stride,
      message.format ?? 0,
      pixels,
    );
    return new ImageData(rgba, message.width, message.height);
  }

  function updateSurfaceCanvas(
    window: HostWindowState,
    imageData: ImageData | null,
  ) {
    window.imageData = imageData;
    if (!imageData) {
      window.surfaceCanvas = null;
      return;
    }

    const surfaceCanvas =
      window.surfaceCanvas ?? document.createElement("canvas");
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

  function drawWindow(window: HostWindowState) {
    if (!window.visible || !window.topLevel || !window.surfaceCanvas) {
      return;
    }

    context2d.drawImage(
      window.surfaceCanvas,
      window.rect.left,
      window.rect.top,
    );
  }

  function composite() {
    resizeCanvas();

    const rect = canvas.getBoundingClientRect();
    context2d.clearRect(0, 0, rect.width, rect.height);
    context2d.fillStyle = PLAYGROUND_BACKGROUND;
    context2d.fillRect(0, 0, rect.width, rect.height);

    for (const window of windows.values()) {
      drawWindow(window);
    }
  }

  function hitTest(clientX: number, clientY: number) {
    let hit: HostWindowState | null = null;
    for (const window of windows.values()) {
      if (!window.visible || !window.enabled || !window.topLevel) {
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

  function sendUiEvent(
    hwnd: number,
    message: number,
    wParam: number,
    lParam: number,
  ) {
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
        window.controlId = message.control_id ?? 0;
        window.clientInsets = message.client_insets ?? {
          left: 0,
          top: 0,
          right: 0,
          bottom: 0,
        };
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
    const window = hitTest(point.x, point.y);
    const target =
      window ?? (focusedWindow ? (windows.get(focusedWindow) ?? null) : null);
    if (!target) {
      return;
    }

    focusedWindow = target.hwnd;

    const localX = point.x - target.rect.left;
    const localY = point.y - target.rect.top;

    sendUiEvent(target.hwnd, message, 0, packPoint(localX, localY));
  }

  function onKey(message: number, event: KeyboardEvent) {
    if (!focusedWindow) {
      return;
    }

    const key =
      event.key.length === 1 ? event.key.toUpperCase().charCodeAt(0) : 0;
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
