plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// The launcher icon is the web page's pinata, so the repository keeps a single copy of the image.
val launcherIconDirectory = layout.buildDirectory.dir("generated/launcher-icon")
val copyLauncherIcon = tasks.register<Copy>("copyLauncherIcon") {
    from(rootProject.file("../../page/public/pinata.png"))
    into(launcherIconDirectory.map { it.dir("drawable-nodpi") })
    rename { "ic_launcher_pinata.png" }
}

tasks.named("preBuild") {
    dependsOn(copyLauncherIcon)
}

android {
    namespace = "dev.sogen.app"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        aidl = true
    }

    defaultConfig {
        applicationId = "dev.sogen.app"
        // AAudio, which the native runtime uses for sound, needs API 26.
        minSdk = maxOf(26, flutter.minSdkVersion)
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    sourceSets {
        // libsogen.so and its dependencies come from the sogen CMake build; see app/README.md.
        getByName("main").jniLibs.srcDir("src/main/jniLibs")
        getByName("main").res.srcDir(launcherIconDirectory)
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("debug")
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
