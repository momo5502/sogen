export function createSogenUiHost(worker, canvas) {
  const context = canvas.getContext('2d');
  const windows = new Map();
  let focusedWindow = 0;

  const WM_KEYDOWN = 0x0100;
  const WM_KEYUP = 0x0101;
  const WM_CHAR = 0x0102;
  const WM_LBUTTONDOWN = 0x0201;
  const WM_LBUTTONUP = 0x0202;
  const WM_RBUTTONDOWN = 0x0204;
  const WM_RBUTTONUP = 0x0205;
  const WM_MOUSEMOVE = 0x0200;

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

  function composite() {
    context.clearRect(0, 0, canvas.width, canvas.height);

    for (const win of windows.values()) {
      if (!win.visible || !win.topLevel || !win.imageData) {
        continue;
      }

      context.putImageData(win.imageData, win.rect.left, win.rect.top);
    }
  }

  function containsPoint(win, x, y) {
    return x >= win.rect.left && x < win.rect.right && y >= win.rect.top && y < win.rect.bottom;
  }

  function findTopLevelAt(x, y) {
    let hit = 0;
    for (const win of windows.values()) {
      if (!win.visible || !win.enabled || !win.topLevel) {
        continue;
      }

      if (containsPoint(win, x, y)) {
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

  canvas.tabIndex = 0;
  canvas.addEventListener('mousedown', (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.floor(event.clientX - rect.left);
    const y = Math.floor(event.clientY - rect.top);
    const hwnd = findTopLevelAt(x, y);
    if (!hwnd) {
      return;
    }

    const win = windows.get(hwnd);
    const localX = x - win.rect.left;
    const localY = y - win.rect.top;
    focusedWindow = hwnd;
    canvas.focus();
    sendEvent(hwnd, event.button === 2 ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, 0, packPoint(localX, localY));
  });

  canvas.addEventListener('mouseup', (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.floor(event.clientX - rect.left);
    const y = Math.floor(event.clientY - rect.top);
    const hwnd = findTopLevelAt(x, y) || focusedWindow;
    if (!hwnd) {
      return;
    }

    const win = windows.get(hwnd);
    const localX = win ? x - win.rect.left : x;
    const localY = win ? y - win.rect.top : y;
    sendEvent(hwnd, event.button === 2 ? WM_RBUTTONUP : WM_LBUTTONUP, 0, packPoint(localX, localY));
  });

  canvas.addEventListener('mousemove', (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.floor(event.clientX - rect.left);
    const y = Math.floor(event.clientY - rect.top);
    const hwnd = findTopLevelAt(x, y) || focusedWindow;
    if (!hwnd) {
      return;
    }

    const win = windows.get(hwnd);
    const localX = win ? x - win.rect.left : x;
    const localY = win ? y - win.rect.top : y;
    sendEvent(hwnd, WM_MOUSEMOVE, 0, packPoint(localX, localY));
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
