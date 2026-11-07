package dev.sogen.app.runtime;

oneway interface ISogenCallback {
    void onOutput(String text, int color);
    void onConsoleOutput(in byte[] data);
    void onWindowCreated(long window, int width, int height, String title, boolean visible);
    void onWindowDestroyed(long window);
    void onWindowResized(long window, int width, int height);
    void onWindowVisibilityChanged(long window, boolean visible);
    void onWindowTitleChanged(long window, String title);
    void onCursorMoved(long window, int x, int y);
    void onRelativeMouseChanged(long window, boolean relative);

    /** [error] is empty once the process was saved to [path]. */
    void onSnapshot(String path, String error);
    void onExit(int exitCode);

    /** The session runs in the service process [processId]. */
    void onProcessStarted(int processId);

    /** The guest created a child process; the launcher answers with [ISogenSession.childStarted]. */
    void startChild(long request, in String[] arguments);
    void killChild(long request);

    /** Frames presented so far, the longest time between two presents since the last report, and the memory in use. */
    void onStats(long frames, long worstPresentIntervalNs, long residentBytes);
}
