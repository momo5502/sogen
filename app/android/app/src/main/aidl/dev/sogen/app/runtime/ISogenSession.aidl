package dev.sogen.app.runtime;

import android.os.Bundle;
import android.view.Surface;
import dev.sogen.app.runtime.ISogenCallback;

oneway interface ISogenSession {
    void start(in Bundle config, ISogenCallback callback);
    void stop();

    void consoleInput(in byte[] data);
    void consoleResize(int columns, int rows);

    void attachSurface(long window, in Surface surface);
    void detachSurface(long window);

    void mouseMove(long window, int x, int y);
    void mouseButton(long window, int button, boolean down, int x, int y);
    void mouseWheel(long window, int delta, int x, int y);
    void key(long window, int keyCode, boolean down);
    void text(long window, String text);
    void close(long window);
}
