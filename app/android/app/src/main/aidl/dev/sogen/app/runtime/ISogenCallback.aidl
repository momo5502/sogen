package dev.sogen.app.runtime;

oneway interface ISogenCallback {
    void onOutput(String text);
    void onWindowCreated(long window, int width, int height, String title, boolean visible);
    void onWindowDestroyed(long window);
    void onWindowResized(long window, int width, int height);
    void onWindowVisibilityChanged(long window, boolean visible);
    void onWindowTitleChanged(long window, String title);
    void onExit(int exitCode);
}
