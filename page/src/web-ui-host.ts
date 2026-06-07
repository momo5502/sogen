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
  style: number;
  exStyle: number;
  visible: boolean;
  enabled: boolean;
  topLevel: boolean;
  imageData: ImageData | null;
  surfaceCanvas: HTMLCanvasElement | null;
}

interface Rect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

interface Point {
  x: number;
  y: number;
}

type CaptionCommand = "minimize" | "maximize" | "close";

interface CaptionButton extends Rect {
  command: CaptionCommand;
}

type HitRegion = "client" | "caption" | "frame" | "button" | "disabled";

interface HitResult {
  window: HostWindowState;
  region: HitRegion;
  button?: CaptionButton;
}

const PLAYGROUND_BACKGROUND = "#18181b";

interface AttachSogenUiHostOptions {
  onWindowCountChanged?: (count: number) => void;
}

const WM_KEYDOWN = 0x0100;
const WM_KEYUP = 0x0101;
const WM_CHAR = 0x0102;
const WM_CLOSE = 0x0010;
const WM_SYSCOMMAND = 0x0112;
const WM_MOUSEMOVE = 0x0200;
const WM_LBUTTONDOWN = 0x0201;
const WM_LBUTTONUP = 0x0202;
const WM_RBUTTONDOWN = 0x0204;
const WM_RBUTTONUP = 0x0205;

const MK_LBUTTON = 0x0001;

const SC_MINIMIZE = 0xf020;
const SC_MAXIMIZE = 0xf030;

// Window style bits relevant to non-client chrome.
const WS_CAPTION = 0x00c00000;
const WS_SYSMENU = 0x00080000;
const WS_MINIMIZEBOX = 0x00020000;
const WS_MAXIMIZEBOX = 0x00010000;

// The guest models windows without a non-client area (client rect == window
// rect), so the presented surface fills win.rect exactly. We draw the caption
// bar and frame *around* win.rect, mirroring how a native window manager (and
// the SDL backend's OS windows) decorate the client area.
const CAPTION_HEIGHT = 30;
const FRAME_BORDER = 1;
const CAPTION_BUTTON_WIDTH = 46;
const VIEWPORT_MARGIN = 16;

const CHROME_COLORS = {
  frame: "#202020",
  captionFocused: "#0078d4",
  captionUnfocused: "#5a5a5a",
  titleFocused: "#ffffff",
  titleUnfocused: "#d8d8d8",
  glyphFocused: "#ffffff",
  glyphUnfocused: "#e0e0e0",
  buttonHover: "rgba(255, 255, 255, 0.18)",
  closeHover: "#e81123",
};

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
  let dragState: { hwnd: number; offsetX: number; offsetY: number } | null =
    null;
  let hoverState: { hwnd: number; command: CaptionCommand } | null = null;
  let viewportOffset: Point = { x: 0, y: 0 };
  let viewportPinnedByUser = false;

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
        style: 0,
        exStyle: 0,
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

  function hasCaption(window: HostWindowState) {
    return (window.style & WS_CAPTION) === WS_CAPTION;
  }

  function hasSysMenu(window: HostWindowState) {
    return (window.style & WS_SYSMENU) !== 0;
  }

  function captionHeight(window: HostWindowState) {
    return hasCaption(window) ? CAPTION_HEIGHT : 0;
  }

  function frameRect(window: HostWindowState): Rect {
    const caption = captionHeight(window);
    return {
      left: window.rect.left - FRAME_BORDER,
      top: window.rect.top - caption - FRAME_BORDER,
      right: window.rect.right + FRAME_BORDER,
      bottom: window.rect.bottom + FRAME_BORDER,
    };
  }

  function captionRect(window: HostWindowState): Rect {
    const frame = frameRect(window);
    return {
      left: frame.left,
      top: frame.top,
      right: frame.right,
      bottom: window.rect.top,
    };
  }

  // Caption buttons, right-aligned: [minimize, maximize, close].
  function captionButtons(window: HostWindowState): CaptionButton[] {
    if (!hasCaption(window) || !hasSysMenu(window)) {
      return [];
    }

    const caption = captionRect(window);
    const top = caption.top + FRAME_BORDER;
    const bottom = caption.bottom;
    const buttons: CaptionButton[] = [];

    const close: CaptionButton = {
      command: "close",
      left: caption.right - FRAME_BORDER - CAPTION_BUTTON_WIDTH,
      right: caption.right - FRAME_BORDER,
      top,
      bottom,
    };
    buttons.push(close);

    if (window.style & WS_MAXIMIZEBOX) {
      buttons.push({
        command: "maximize",
        left: close.left - CAPTION_BUTTON_WIDTH,
        right: close.left,
        top,
        bottom,
      });
    }

    if (window.style & WS_MINIMIZEBOX) {
      const reference = buttons[buttons.length - 1];
      buttons.push({
        command: "minimize",
        left: reference.left - CAPTION_BUTTON_WIDTH,
        right: reference.left,
        top,
        bottom,
      });
    }

    return buttons;
  }

  function pointInRect(rect: Rect, x: number, y: number) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
  }

  function orderedTopLevelWindows() {
    const ordered: HostWindowState[] = [];
    for (const window of windows.values()) {
      if (window.visible && window.topLevel) {
        ordered.push(window);
      }
    }

    return ordered;
  }

  function visibleWindowBounds(windowsToDraw: HostWindowState[]) {
    if (windowsToDraw.length === 0) {
      return null;
    }

    const bounds = {
      left: Number.POSITIVE_INFINITY,
      top: Number.POSITIVE_INFINITY,
      right: Number.NEGATIVE_INFINITY,
      bottom: Number.NEGATIVE_INFINITY,
    };

    for (const window of windowsToDraw) {
      const frame = frameRect(window);
      bounds.left = Math.min(bounds.left, frame.left);
      bounds.top = Math.min(bounds.top, frame.top);
      bounds.right = Math.max(bounds.right, frame.right);
      bounds.bottom = Math.max(bounds.bottom, frame.bottom);
    }

    return bounds;
  }

  function computeViewportAxisOffset(
    min: number,
    max: number,
    viewportSize: number,
  ) {
    const size = max - min;
    const usableSize = Math.max(1, viewportSize - VIEWPORT_MARGIN * 2);

    if (size <= usableSize) {
      if (min < VIEWPORT_MARGIN || max > viewportSize - VIEWPORT_MARGIN) {
        return Math.round((viewportSize - size) / 2 - min);
      }

      return 0;
    }

    if (min > VIEWPORT_MARGIN) {
      return Math.round(VIEWPORT_MARGIN - min);
    }

    if (max < viewportSize - VIEWPORT_MARGIN) {
      return Math.round(viewportSize - VIEWPORT_MARGIN - max);
    }

    return 0;
  }

  function updateViewportOffset(
    windowsToDraw: HostWindowState[],
    viewportWidth: number,
    viewportHeight: number,
  ) {
    const bounds = visibleWindowBounds(windowsToDraw);
    if (!bounds) {
      viewportOffset = { x: 0, y: 0 };
      return;
    }

    viewportOffset = {
      x: computeViewportAxisOffset(bounds.left, bounds.right, viewportWidth),
      y: computeViewportAxisOffset(bounds.top, bounds.bottom, viewportHeight),
    };
  }

  function drawButtonGlyph(button: CaptionButton, color: string) {
    const cx = (button.left + button.right) / 2;
    const cy = (button.top + button.bottom) / 2;

    context2d.save();
    context2d.strokeStyle = color;
    context2d.lineWidth = 1;
    context2d.beginPath();

    if (button.command === "close") {
      context2d.moveTo(cx - 5, cy - 5);
      context2d.lineTo(cx + 5, cy + 5);
      context2d.moveTo(cx + 5, cy - 5);
      context2d.lineTo(cx - 5, cy + 5);
    } else if (button.command === "maximize") {
      context2d.strokeRect(
        Math.round(cx - 5) + 0.5,
        Math.round(cy - 5) + 0.5,
        10,
        10,
      );
    } else {
      context2d.moveTo(cx - 5, Math.round(cy) + 0.5);
      context2d.lineTo(cx + 5, Math.round(cy) + 0.5);
    }

    context2d.stroke();
    context2d.restore();
  }

  function drawChrome(window: HostWindowState) {
    if (!hasCaption(window)) {
      return;
    }

    const frame = frameRect(window);
    const caption = captionRect(window);
    const focused = window.hwnd === focusedWindow;

    context2d.fillStyle = CHROME_COLORS.frame;
    context2d.fillRect(
      frame.left,
      frame.top,
      frame.right - frame.left,
      frame.bottom - frame.top,
    );

    context2d.fillStyle = focused
      ? CHROME_COLORS.captionFocused
      : CHROME_COLORS.captionUnfocused;
    context2d.fillRect(
      caption.left,
      caption.top,
      caption.right - caption.left,
      caption.bottom - caption.top,
    );

    const buttons = captionButtons(window);

    for (const button of buttons) {
      const hovered =
        hoverState !== null &&
        hoverState.hwnd === window.hwnd &&
        hoverState.command === button.command;
      if (hovered) {
        context2d.fillStyle =
          button.command === "close"
            ? CHROME_COLORS.closeHover
            : CHROME_COLORS.buttonHover;
        context2d.fillRect(
          button.left,
          button.top,
          button.right - button.left,
          button.bottom - button.top,
        );
      }

      const glyphColor =
        hovered && button.command === "close"
          ? "#ffffff"
          : focused
            ? CHROME_COLORS.glyphFocused
            : CHROME_COLORS.glyphUnfocused;
      drawButtonGlyph(button, glyphColor);
    }

    const titleRight = buttons.length
      ? buttons[buttons.length - 1].left
      : caption.right - FRAME_BORDER;
    const available = titleRight - (caption.left + 10);
    if (window.title && available > 8) {
      context2d.save();
      context2d.beginPath();
      context2d.rect(
        caption.left + 8,
        caption.top,
        available,
        caption.bottom - caption.top,
      );
      context2d.clip();
      context2d.fillStyle = focused
        ? CHROME_COLORS.titleFocused
        : CHROME_COLORS.titleUnfocused;
      context2d.font = '13px "Segoe UI", system-ui, sans-serif';
      context2d.textBaseline = "middle";
      context2d.fillText(
        window.title,
        caption.left + 10,
        (caption.top + caption.bottom) / 2 + 1,
      );
      context2d.restore();
    }
  }

  function drawWindow(window: HostWindowState) {
    if (!window.visible || !window.topLevel) {
      return;
    }

    drawChrome(window);

    if (window.surfaceCanvas) {
      context2d.drawImage(
        window.surfaceCanvas,
        window.rect.left,
        window.rect.top,
      );
    }
  }

  function composite() {
    resizeCanvas();

    const rect = canvas.getBoundingClientRect();
    context2d.clearRect(0, 0, rect.width, rect.height);
    context2d.fillStyle = PLAYGROUND_BACKGROUND;
    context2d.fillRect(0, 0, rect.width, rect.height);

    const windowsToDraw = orderedTopLevelWindows();
    if (!viewportPinnedByUser) {
      updateViewportOffset(windowsToDraw, rect.width, rect.height);
    }

    context2d.save();
    context2d.translate(viewportOffset.x, viewportOffset.y);
    for (const window of windowsToDraw) {
      drawWindow(window);
    }
    context2d.restore();
  }

  function raiseWindow(hwnd: number) {
    const window = windows.get(hwnd);
    if (!window) {
      return;
    }
    // Re-insert to move to the end of the Map so it composites on top.
    windows.delete(hwnd);
    windows.set(hwnd, window);
  }

  function hitTest(x: number, y: number): HitResult | null {
    const ordered = orderedTopLevelWindows();

    for (let i = ordered.length - 1; i >= 0; --i) {
      const window = ordered[i];
      if (!window.enabled) {
        if (pointInRect(frameRect(window), x, y)) {
          return { window, region: "disabled" };
        }
        continue;
      }

      if (pointInRect(window.rect, x, y)) {
        return { window, region: "client" };
      }

      for (const button of captionButtons(window)) {
        if (pointInRect(button, x, y)) {
          return { window, region: "button", button };
        }
      }

      if (pointInRect(captionRect(window), x, y)) {
        return { window, region: "caption" };
      }

      if (pointInRect(frameRect(window), x, y)) {
        return { window, region: "frame" };
      }
    }

    return null;
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
      x: Math.floor(event.clientX - rect.left - viewportOffset.x),
      y: Math.floor(event.clientY - rect.top - viewportOffset.y),
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
    console.debug("[sogen-ui][recv]", message.command, "hwnd=" + hwnd); // TEMP diagnostic
    if (!hwnd) {
      return;
    }

    switch (message.command) {
      case "create_window": {
        viewportPinnedByUser = false;
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
        window.style = message.style ?? 0;
        window.exStyle = message.ex_style ?? 0;
        window.visible = !!message.visible;
        window.enabled = message.enabled ?? true;
        window.topLevel = message.top_level ?? true;
        if (window.topLevel && window.visible) {
          focusedWindow = hwnd;
        }
        break;
      }
      case "destroy_window":
        viewportPinnedByUser = false;
        windows.delete(hwnd);
        notifyWindowCount();
        if (focusedWindow === hwnd) {
          focusedWindow = 0;
        }
        break;
      case "set_rect":
        viewportPinnedByUser = false;
        getWindow(hwnd).rect = cloneRect(message.rect);
        break;
      case "set_visible":
        viewportPinnedByUser = false;
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

  function triggerButton(window: HostWindowState, button: CaptionButton) {
    if (button.command === "close") {
      sendUiEvent(window.hwnd, WM_CLOSE, 0, 0);
    } else if (button.command === "minimize") {
      sendUiEvent(window.hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    } else {
      sendUiEvent(window.hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    }
  }

  function onMouseDown(event: MouseEvent) {
    canvas.focus();
    const point = toCanvasPoint(event);
    const hit = hitTest(point.x, point.y);
    if (!hit || hit.region === "disabled") {
      return;
    }

    const window = hit.window;
    focusedWindow = window.hwnd;
    raiseWindow(window.hwnd);

    if (hit.region === "button") {
      composite();
      return;
    }

    if (hit.region === "caption" || hit.region === "frame") {
      if (event.button === 0) {
        viewportPinnedByUser = true;
        dragState = {
          hwnd: window.hwnd,
          offsetX: point.x - window.rect.left,
          offsetY: point.y - window.rect.top,
        };
      }
      composite();
      return;
    }

    const localX = point.x - window.rect.left;
    const localY = point.y - window.rect.top;
    sendUiEvent(
      window.hwnd,
      event.button === 2 ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
      event.button === 0 ? MK_LBUTTON : 0,
      packPoint(localX, localY),
    );
    composite();
  }

  function onMouseUp(event: MouseEvent) {
    const point = toCanvasPoint(event);

    if (dragState) {
      dragState = null;
      return;
    }

    const hit = hitTest(point.x, point.y);
    if (hit && hit.region === "button" && hit.button && event.button === 0) {
      triggerButton(hit.window, hit.button);
      return;
    }

    const target =
      hit && hit.region === "client"
        ? hit.window
        : focusedWindow
          ? (windows.get(focusedWindow) ?? null)
          : null;
    if (!target) {
      return;
    }

    const localX = point.x - target.rect.left;
    const localY = point.y - target.rect.top;
    sendUiEvent(
      target.hwnd,
      event.button === 2 ? WM_RBUTTONUP : WM_LBUTTONUP,
      0,
      packPoint(localX, localY),
    );
  }

  function onMouseMove(event: MouseEvent) {
    const point = toCanvasPoint(event);

    if (dragState) {
      const window = windows.get(dragState.hwnd);
      if (window) {
        const width = window.rect.right - window.rect.left;
        const height = window.rect.bottom - window.rect.top;
        window.rect = {
          left: point.x - dragState.offsetX,
          top: point.y - dragState.offsetY,
          right: point.x - dragState.offsetX + width,
          bottom: point.y - dragState.offsetY + height,
        };
        composite();
      }
      return;
    }

    const hit = hitTest(point.x, point.y);
    let nextHover: { hwnd: number; command: CaptionCommand } | null = null;
    if (hit && hit.region === "button" && hit.button) {
      nextHover = { hwnd: hit.window.hwnd, command: hit.button.command };
    }

    const hoverChanged =
      (nextHover?.hwnd ?? 0) !== (hoverState?.hwnd ?? 0) ||
      (nextHover?.command ?? "") !== (hoverState?.command ?? "");
    if (hoverChanged) {
      hoverState = nextHover;
      composite();
    }

    if (hit && hit.region === "client") {
      const localX = point.x - hit.window.rect.left;
      const localY = point.y - hit.window.rect.top;
      sendUiEvent(hit.window.hwnd, WM_MOUSEMOVE, 0, packPoint(localX, localY));
    }
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
  canvas.addEventListener("mousedown", onMouseDown);
  canvas.addEventListener("mouseup", onMouseUp);
  canvas.addEventListener("mousemove", onMouseMove);
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
