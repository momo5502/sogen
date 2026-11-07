package dev.sogen.app.runtime;

import android.os.Bundle;
import android.view.Surface;
import dev.sogen.app.runtime.ISogenCallback;

oneway interface ISogenSession {
    void start(in Bundle config, ISogenCallback callback);
    void stop();

    /** Loads the emulator ahead of [start], so that a child process waits less for it. */
    void prepare();

    /** [processId] is the child's service process, 0 or less if it could not be started and -1 if no slot was free. */
    void childStarted(long request, int processId);
    void childExited(long request, int exitStatus);

    /** Saves the process to [destination], or to a new file when it is empty, and ends the session. */
    void suspend(String destination);

    void consoleInput(in byte[] data);
    void consoleResize(int columns, int rows);

    void attachSurface(long window, in Surface surface);
    void detachSurface(long window);

    void mouseMove(long window, int x, int y);
    void mouseMotion(long window, int dx, int dy);
    void mouseButton(long window, int button, boolean down, int x, int y);
    void mouseWheel(long window, int delta, int x, int y);
    void key(long window, int keyCode, boolean down);
    void text(long window, String text);
    void close(long window);

    /** 0 turns the limit off. */
    void setFrameLimit(int framesPerSecond);

    /** While [milliseconds] is above 0, [ISogenCallback.onStats] reports that often. */
    void setStatsInterval(int milliseconds);
}
