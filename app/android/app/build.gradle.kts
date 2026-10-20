plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// The launcher icon is the web page's pinata, so the repository keeps a single copy of the image.
abstract class CopyLauncherIcon : DefaultTask() {
    @get:InputFile
    abstract val image: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun copy() {
        val target = outputDirectory.get().dir("drawable-nodpi").file("ic_launcher_pinata.png").asFile
        target.parentFile.mkdirs()
        image.get().asFile.copyTo(target, overwrite = true)
    }
}

val copyLauncherIcon = tasks.register<CopyLauncherIcon>("copyLauncherIcon") {
    image.set(rootProject.layout.projectDirectory.file("../../page/public/pinata.png"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.res?.addGeneratedSourceDirectory(copyLauncherIcon, CopyLauncherIcon::outputDirectory)
    }
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
