export function createSogenUiHost(worker, canvas) {
  const context = canvas.getContext('2d');
  const windows = new Map();
  let focusedWindow = 0;
  let dragState = null;
  let hoverState = null;

  const WM_KEYDOWN = 0x0100;
  const WM_KEYUP = 0x0101;
  const WM_CHAR = 0x0102;
  const WM_CLOSE = 0x0010;
  const WM_SYSCOMMAND = 0x0112;
  const WM_LBUTTONDOWN = 0x0201;
  const WM_LBUTTONUP = 0x0202;
  const WM_RBUTTONDOWN = 0x0204;
  const WM_RBUTTONUP = 0x0205;
  const WM_MOUSEMOVE = 0x0200;

  const MK_LBUTTON = 0x0001;

  const SC_MINIMIZE = 0xf020;
  const SC_MAXIMIZE = 0xf030;

  // Window-manager chrome metrics. The emulator models windows without a
  // non-client area (the guest client rect equals the window rect), so the
  // presented surface fills win.rect exactly. We therefore draw the caption
  // bar and frame *around* win.rect, mirroring how a native window manager
  // (and the SDL backend's OS windows) decorate the client area.
  const CAPTION_HEIGHT = 30;
  const BORDER = 1;
  const BUTTON_WIDTH = 46;

  const COLORS = {
    frame: '#202020',
    captionFocused: '#0078d4',
    captionUnfocused: '#9b9b9b',
    titleFocused: '#ffffff',
    titleUnfocused: '#f0f0f0',
    glyphFocused: '#ffffff',
    glyphUnfocused: '#e8e8e8',
    buttonHover: 'rgba(255, 255, 255, 0.18)',
    closeHover: '#e81123',
  };

  function getWindow(hwnd) {
    let win = windows.get(hwnd);
    if (!win) {
      win = {
        hwnd,
        parent: 0,
        owner: 0,
        rect: { left: 0, top: 0, right: 0, bottom: 0 },
        clientInsets: { left: 0, top: 0, right: 0, bottom: 0 },
        className: '',
        title: '',
        style: 0,
        exStyle: 0,
        controlId: 0,
        topLevel: false,
        visible: false,
        enabled: true,
        imageData: null,
      };
      windows.set(hwnd, win);
    }
    return win;
  }

  // WS_CAPTION = 0x00C00000, WS_SYSMENU = 0x00080000,
  // WS_MINIMIZEBOX = 0x00020000, WS_MAXIMIZEBOX = 0x00010000.
  function hasCaption(win) {
    return (win.style & 0x00c00000) === 0x00c00000;
  }

  function hasSysMenu(win) {
    return (win.style & 0x00080000) !== 0;
  }

  function captionHeight(win) {
    return hasCaption(win) ? CAPTION_HEIGHT : 0;
  }

  // Outer frame rect (caption + border) surrounding the client rect.
  function frameRect(win) {
    const caption = captionHeight(win);
    return {
      left: win.rect.left - BORDER,
      top: win.rect.top - caption - BORDER,
      right: win.rect.right + BORDER,
      bottom: win.rect.bottom + BORDER,
    };
  }

  function captionRect(win) {
    const frame = frameRect(win);
    return {
      left: frame.left,
      top: frame.top,
      right: frame.right,
      bottom: win.rect.top,
    };
  }

  // Caption buttons, right-aligned: [minimize, maximize, close].
  function captionButtons(win) {
    if (!hasCaption(win) || !hasSysMenu(win)) {
      return [];
    }

    const caption = captionRect(win);
    const top = caption.top + BORDER;
    const bottom = caption.bottom;
    const buttons = [];

    const close = {
      command: 'close',
      left: caption.right - BORDER - BUTTON_WIDTH,
      right: caption.right - BORDER,
      top,
      bottom,
    };
    buttons.push(close);

    if (win.style & 0x00010000) {
      buttons.push({
        command: 'maximize',
        left: close.left - BUTTON_WIDTH,
        right: close.left,
        top,
        bottom,
      });
    }

    if (win.style & 0x00020000) {
      const reference = buttons[buttons.length - 1];
      buttons.push({
        command: 'minimize',
        left: reference.left - BUTTON_WIDTH,
        right: reference.left,
        top,
        bottom,
      });
    }

    return buttons;
  }

  function pointInRect(rect, x, y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
  }

  function drawButtonGlyph(button, color) {
    const cx = (button.left + button.right) / 2;
    const cy = (button.top + button.bottom) / 2;

    context.save();
    context.strokeStyle = color;
    context.lineWidth = 1;
    context.beginPath();

    if (button.command === 'close') {
      context.moveTo(cx - 5, cy - 5);
      context.lineTo(cx + 5, cy + 5);
      context.moveTo(cx + 5, cy - 5);
      context.lineTo(cx - 5, cy + 5);
    } else if (button.command === 'maximize') {
      context.rect(Math.round(cx - 5) + 0.5, Math.round(cy - 5) + 0.5, 10, 10);
    } else {
      context.moveTo(cx - 5, Math.round(cy) + 0.5);
      context.lineTo(cx + 5, Math.round(cy) + 0.5);
    }

    context.stroke();
    context.restore();
  }

  function drawChrome(win) {
    if (!hasCaption(win)) {
      return;
    }

    const frame = frameRect(win);
    const caption = captionRect(win);
    const focused = win.hwnd === focusedWindow;

    // Frame border.
    context.fillStyle = COLORS.frame;
    context.fillRect(frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top);

    // Caption background.
    context.fillStyle = focused ? COLORS.captionFocused : COLORS.captionUnfocused;
    context.fillRect(caption.left, caption.top, caption.right - caption.left, caption.bottom - caption.top);

    const buttons = captionButtons(win);

    // Button backgrounds (hover) + glyphs.
    for (const button of buttons) {
      const hovered =
        hoverState && hoverState.hwnd === win.hwnd && hoverState.command === button.command;
      if (hovered) {
        context.fillStyle = button.command === 'close' ? COLORS.closeHover : COLORS.buttonHover;
        context.fillRect(button.left, button.top, button.right - button.left, button.bottom - button.top);
      }

      const glyphColor =
        hovered && button.command === 'close'
          ? '#ffffff'
          : focused
            ? COLORS.glyphFocused
            : COLORS.glyphUnfocused;
      drawButtonGlyph(button, glyphColor);
    }

    // Title text.
    const titleRight = buttons.length ? buttons[buttons.length - 1].left : caption.right - BORDER;
    const available = titleRight - (caption.left + 10);
    if (win.title && available > 8) {
      context.save();
      context.beginPath();
      context.rect(caption.left + 8, caption.top, available, caption.bottom - caption.top);
      context.clip();
      context.fillStyle = focused ? COLORS.titleFocused : COLORS.titleUnfocused;
      context.font = '13px "Segoe UI", system-ui, sans-serif';
      context.textBaseline = 'middle';
      context.fillText(win.title, caption.left + 10, (caption.top + caption.bottom) / 2 + 1);
      context.restore();
    }
  }

  function orderedWindows() {
    const result = [];
    for (const win of windows.values()) {
      if (win.visible && win.topLevel) {
        result.push(win);
      }
    }
    return result;
  }

  function composite() {
    context.clearRect(0, 0, canvas.width, canvas.height);

    for (const win of orderedWindows()) {
      drawChrome(win);
      if (win.imageData) {
        context.putImageData(win.imageData, win.rect.left, win.rect.top);
      }
    }
  }

  function raiseWindow(hwnd) {
    const win = windows.get(hwnd);
    if (!win) {
      return;
    }
    // Re-insert to move to the end of the Map so it composites on top.
    windows.delete(hwnd);
    windows.set(hwnd, win);
  }

  // Hit-test top-level windows (including their chrome) front-to-back.
  function hitTest(x, y) {
    const ordered = orderedWindows();
    for (let i = ordered.length - 1; i >= 0; --i) {
      const win = ordered[i];
      if (!win.enabled) {
        if (pointInRect(frameRect(win), x, y)) {
          return { win, region: 'disabled' };
        }
        continue;
      }

      if (pointInRect(win.rect, x, y)) {
        return { win, region: 'client' };
      }

      for (const button of captionButtons(win)) {
        if (pointInRect(button, x, y)) {
          return { win, region: 'button', button };
        }
      }

      if (pointInRect(captionRect(win), x, y)) {
        return { win, region: 'caption' };
      }

      if (pointInRect(frameRect(win), x, y)) {
        return { win, region: 'frame' };
      }
    }

    return null;
  }

  function packPoint(x, y) {
    return ((y & 0xffff) << 16) | (x & 0xffff);
  }

  function sendEvent(hwnd, message, wParam, lParam) {
    worker.postMessage({
      type: 'sogen_ui_event',
      window: hwnd >>> 0,
      message: message >>> 0,
      wParam: wParam >>> 0,
      lParam: lParam >>> 0,
    });
  }

  function triggerButton(win, button) {
    if (button.command === 'close') {
      sendEvent(win.hwnd, WM_CLOSE, 0, 0);
    } else if (button.command === 'minimize') {
      sendEvent(win.hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    } else if (button.command === 'maximize') {
      sendEvent(win.hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    }
  }

  function updateWindowFromMessage(win, message) {
    win.parent = message.parent >>> 0;
    win.owner = message.owner >>> 0;
    win.rect = message.rect;
    win.clientInsets = message.client_insets || { left: 0, top: 0, right: 0, bottom: 0 };
    win.className = message.class_name || '';
    win.title = message.title || '';
    win.style = message.style >>> 0;
    win.exStyle = message.ex_style >>> 0;
    win.controlId = message.control_id >>> 0;
    win.visible = !!message.visible;
    win.enabled = !!message.enabled;
    win.topLevel = !!message.top_level;
  }

  function convertSurface(message) {
    const width = message.width | 0;
    const height = message.height | 0;
    const stride = message.stride | 0;
    const format = message.format | 0;
    const source = new Uint8Array(message.pixels);
    const pixels = new Uint8ClampedArray(width * height * 4);

    for (let y = 0; y < height; ++y) {
      const sourceBase = y * stride;
      const destBase = y * width * 4;

      for (let x = 0; x < width; ++x) {
        const sourceIndex = sourceBase + x * 4;
        const destIndex = destBase + x * 4;

        if (format === 0) {
          pixels[destIndex] = source[sourceIndex + 2];
          pixels[destIndex + 1] = source[sourceIndex + 1];
          pixels[destIndex + 2] = source[sourceIndex];
          pixels[destIndex + 3] = source[sourceIndex + 3];
        } else {
          pixels[destIndex] = source[sourceIndex];
          pixels[destIndex + 1] = source[sourceIndex + 1];
          pixels[destIndex + 2] = source[sourceIndex + 2];
          pixels[destIndex + 3] = source[sourceIndex + 3];
        }
      }
    }

    return new ImageData(pixels, width, height);
  }

  worker.addEventListener('message', (event) => {
    const message = event.data;
    if (!message || message.type !== 'sogen_ui') {
      return;
    }

    switch (message.command) {
      case 'create_window': {
        const win = getWindow(message.hwnd >>> 0);
        updateWindowFromMessage(win, message);
        if (win.topLevel && win.visible) {
          focusedWindow = win.hwnd;
        }
        break;
      }
      case 'destroy_window':
        windows.delete(message.hwnd >>> 0);
        if (focusedWindow === (message.hwnd >>> 0)) {
          focusedWindow = 0;
        }
        break;
      case 'set_rect':
        getWindow(message.hwnd >>> 0).rect = message.rect;
        break;
      case 'set_visible':
        getWindow(message.hwnd >>> 0).visible = !!message.value;
        break;
      case 'set_enabled':
        getWindow(message.hwnd >>> 0).enabled = !!message.value;
        break;
      case 'set_title':
        getWindow(message.hwnd >>> 0).title = message.title;
        break;
      case 'present_surface': {
        const win = getWindow(message.hwnd >>> 0);
        win.imageData = convertSurface(message);
        break;
      }
      default:
        break;
    }

    composite();
  });

  function canvasPoint(event) {
    const rect = canvas.getBoundingClientRect();
    return {
      x: Math.floor(event.clientX - rect.left),
      y: Math.floor(event.clientY - rect.top),
    };
  }

  canvas.tabIndex = 0;
  canvas.addEventListener('mousedown', (event) => {
    const { x, y } = canvasPoint(event);
    const hit = hitTest(x, y);
    if (!hit || hit.region === 'disabled') {
      return;
    }

    const win = hit.win;
    focusedWindow = win.hwnd;
    raiseWindow(win.hwnd);
    canvas.focus();

    if (hit.region === 'button') {
      composite();
      return;
    }

    if (hit.region === 'caption' || hit.region === 'frame') {
      if (event.button === 0) {
        dragState = { hwnd: win.hwnd, offsetX: x - win.rect.left, offsetY: y - win.rect.top };
      }
      composite();
      return;
    }

    const localX = x - win.rect.left;
    const localY = y - win.rect.top;
    sendEvent(win.hwnd, event.button === 2 ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, event.button === 0 ? MK_LBUTTON : 0, packPoint(localX, localY));
    composite();
  });

  canvas.addEventListener('mouseup', (event) => {
    const { x, y } = canvasPoint(event);

    if (dragState) {
      dragState = null;
      return;
    }

    const hit = hitTest(x, y);
    if (hit && hit.region === 'button' && event.button === 0) {
      triggerButton(hit.win, hit.button);
      return;
    }

    const target = hit && (hit.region === 'client') ? hit.win : windows.get(focusedWindow);
    if (!target) {
      return;
    }

    const localX = x - target.rect.left;
    const localY = y - target.rect.top;
    sendEvent(target.hwnd, event.button === 2 ? WM_RBUTTONUP : WM_LBUTTONUP, 0, packPoint(localX, localY));
  });

  canvas.addEventListener('mousemove', (event) => {
    const { x, y } = canvasPoint(event);

    if (dragState) {
      const win = windows.get(dragState.hwnd);
      if (win) {
        const width = win.rect.right - win.rect.left;
        const height = win.rect.bottom - win.rect.top;
        win.rect = {
          left: x - dragState.offsetX,
          top: y - dragState.offsetY,
          right: x - dragState.offsetX + width,
          bottom: y - dragState.offsetY + height,
        };
        composite();
      }
      return;
    }

    const hit = hitTest(x, y);
    const nextHover = hit && hit.region === 'button' ? { hwnd: hit.win.hwnd, command: hit.button.command } : null;
    const hoverChanged =
      (!!nextHover !== !!hoverState) ||
      (nextHover && hoverState && (nextHover.hwnd !== hoverState.hwnd || nextHover.command !== hoverState.command));
    if (hoverChanged) {
      hoverState = nextHover;
      composite();
    }

    if (hit && hit.region === 'client') {
      const localX = x - hit.win.rect.left;
      const localY = y - hit.win.rect.top;
      sendEvent(hit.win.hwnd, WM_MOUSEMOVE, 0, packPoint(localX, localY));
    }
  });

  canvas.addEventListener('keydown', (event) => {
    if (!focusedWindow) {
      return;
    }

    const key = event.key && event.key.length === 1 ? event.key.toUpperCase().charCodeAt(0) : 0;
    const wParam = key || event.keyCode || 0;
    sendEvent(focusedWindow, WM_KEYDOWN, wParam, 0);
  });

  canvas.addEventListener('keyup', (event) => {
    if (!focusedWindow) {
      return;
    }

    const key = event.key && event.key.length === 1 ? event.key.toUpperCase().charCodeAt(0) : 0;
    const wParam = key || event.keyCode || 0;
    sendEvent(focusedWindow, WM_KEYUP, wParam, 0);
  });

  canvas.addEventListener('keypress', (event) => {
    if (!focusedWindow || !event.key || event.key.length !== 1) {
      return;
    }

    sendEvent(focusedWindow, WM_CHAR, event.key.charCodeAt(0), 0);
  });

  canvas.addEventListener('contextmenu', (event) => {
    event.preventDefault();
  });

  return {
    composite,
    windows,
  };
}
