# libsogen.so looks these up by name through JNI.
-keep class dev.sogen.app.runtime.SogenNative { native <methods>; }
-keep interface dev.sogen.app.runtime.NativeListener { *; }
-keep class * implements dev.sogen.app.runtime.NativeListener { *; }

# play-services-ads brings in WorkManager, whose database Room creates through the generated subclass's no-argument
# constructor by reflection. The consumer rule of that Room version (2.2.5) keeps the subclass without its constructor,
# which R8 in full mode then removes, so the app crashed on startup.
-keep class * extends androidx.room.RoomDatabase { <init>(); }
