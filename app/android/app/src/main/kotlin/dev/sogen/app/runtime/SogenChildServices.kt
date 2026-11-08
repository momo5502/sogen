package dev.sogen.app.runtime

/**
 * The service processes the child processes of a game run in, one each. A process of the app can only show windows when
 * Android started it as one of the app's components, and every service runs in the process its manifest entry names, so
 * each slot needs a component of its own.
 */
class SogenChildService0 : SogenService()

class SogenChildService1 : SogenService()

val childServices = listOf(SogenChildService0::class.java, SogenChildService1::class.java)
