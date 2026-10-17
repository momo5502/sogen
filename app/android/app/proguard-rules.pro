# libsogen.so looks these up by name through JNI.
-keep class dev.sogen.app.runtime.SogenNative { native <methods>; }
-keep interface dev.sogen.app.runtime.NativeListener { *; }
-keep class * implements dev.sogen.app.runtime.NativeListener { *; }
