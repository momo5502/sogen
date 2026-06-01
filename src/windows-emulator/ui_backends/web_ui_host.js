export function createSogenUiHost(worker, canvas) {
  const context = canvas.getContext('2d');
  const windows = new Map();
  let focusedWindow = 0;

  function getWindow(hwnd) {
    let win = windows.get(hwnd);
    if (!win) {
      win = {
        hwnd,
        rect: { left: 0, top: 0, right: 0, bottom: 0 },
        title: '',
        visible: false,
        enabled: true,
        imageData: null,
      };
      windows.set(hwnd, win);
    }
    return win;
  }

  function composite() {
    context.clearRect(0, 0, canvas.width, canvas.height);

    for (const win of windows.values()) {
      if (!win.visible || !win.imageData) {
        continue;
      }

      context.putImageData(win.imageData, win.rect.left, win.rect.top);
    }
  }

  function hitTest(x, y) {
    let hit = 0;
    for (const win of windows.values()) {
      if (!win.visible) {
        continue;
      }

      if (x >= win.rect.left && x < win.rect.right && y >= win.rect.top && y < win.rect.bottom) {
        hit = win.hwnd;
      }
    }

    return hit;
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

  worker.addEventListener('message', (event) => {
    const message = event.data;
    if (!message || message.type !== 'sogen_ui') {
      return;
    }

    switch (message.command) {
      case 'create_window': {
        const win = getWindow(message.hwnd >>> 0);
        win.rect = message.rect;
        win.title = message.title;
        win.visible = !!message.visible;
        win.enabled = !!message.enabled;
        break;
      }
      case 'destroy_window':
        windows.delete(message.hwnd >>> 0);
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
        const pixels = new Uint8ClampedArray(message.pixels);
        win.imageData = new ImageData(pixels, message.width, message.height);
        break;
      }
      default:
        break;
    }

    composite();
  });

  canvas.tabIndex = 0;
  canvas.addEventListener('mousedown', (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.floor(event.clientX - rect.left);
    const y = Math.floor(event.clientY - rect.top);
    const hwnd = hitTest(x, y);
    if (!hwnd) {
      return;
    }

    focusedWindow = hwnd;
    canvas.focus();
    sendEvent(hwnd, event.button === 2 ? 0x0204 : 0x0201, 0, packPoint(x, y));
  });

  canvas.addEventListener('mouseup', (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.floor(event.clientX - rect.left);
    const y = Math.floor(event.clientY - rect.top);
    const hwnd = hitTest(x, y) || focusedWindow;
    if (!hwnd) {
      return;
    }

    sendEvent(hwnd, event.button === 2 ? 0x0205 : 0x0202, 0, packPoint(x, y));
  });

  canvas.addEventListener('mousemove', (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.floor(event.clientX - rect.left);
    const y = Math.floor(event.clientY - rect.top);
    const hwnd = hitTest(x, y) || focusedWindow;
    if (!hwnd) {
      return;
    }

    sendEvent(hwnd, 0x0200, 0, packPoint(x, y));
  });

  canvas.addEventListener('keydown', (event) => {
    if (!focusedWindow) {
      return;
    }

    const key = event.key && event.key.length === 1 ? event.key.toUpperCase().charCodeAt(0) : 0;
    const wParam = key || event.keyCode || 0;
    sendEvent(focusedWindow, 0x0100, wParam, 0);
  });

  canvas.addEventListener('keyup', (event) => {
    if (!focusedWindow) {
      return;
    }

    const key = event.key && event.key.length === 1 ? event.key.toUpperCase().charCodeAt(0) : 0;
    const wParam = key || event.keyCode || 0;
    sendEvent(focusedWindow, 0x0101, wParam, 0);
  });

  canvas.addEventListener('keypress', (event) => {
    if (!focusedWindow || !event.key || event.key.length !== 1) {
      return;
    }

    sendEvent(focusedWindow, 0x0102, event.key.charCodeAt(0), 0);
  });

  canvas.addEventListener('contextmenu', (event) => {
    event.preventDefault();
  });

  return {
    composite,
    windows,
  };
}
