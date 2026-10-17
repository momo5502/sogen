plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
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

    packaging {
        jniLibs {
            // libadrenotools loads its hook libraries as files from the native library directory, so they
            // have to be extracted on install rather than mapped from the APK.
            useLegacyPackaging = true
        }
    }

    androidResources {
        // The Turnip driver is copied out of the APK with AssetManager.openFd, which needs it uncompressed.
        noCompress += "so"
    }

    sourceSets {
        // libsogen.so and its dependencies come from the sogen CMake build; see app/README.md.
        getByName("main").jniLibs.srcDir("src/main/jniLibs")
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("debug")
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
