import java.util.Properties
import javax.xml.parsers.DocumentBuilderFactory
import org.w3c.dom.Element

plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// The launcher icon is the web page's logo, so the repository keeps a single copy of it. Android has no SVG
// drawables, so the logo's shapes are converted to a vector drawable, centered in a square viewport because a
// vector drawable stretches its viewport to the icon's bounds.
abstract class GenerateLauncherIcon : DefaultTask() {
    @get:InputFile
    abstract val logo: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun generate() {
        val svg = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(logo.get().asFile).documentElement
        val (x, y, width, height) = svg.getAttribute("viewBox").trim().split(Regex("[\\s,]+")).map { it.toDouble() }
        val size = maxOf(width, height)

        val elementsById = mutableMapOf<String, Element>()
        val allElements = svg.getElementsByTagName("*")
        for (i in 0 until allElements.length) {
            val element = allElements.item(i) as Element
            if (element.hasAttribute("id")) {
                elementsById[element.getAttribute("id")] = element
            }
        }

        val paths = StringBuilder()
        fun addPath(element: Element, data: String) {
            val fill = element.getAttribute("fill").ifEmpty { "#000000" }
            val fillType = if (element.getAttribute("fill-rule") == "evenodd") "evenOdd" else "nonZero"
            paths.append("""        <path android:fillColor="$fill" android:fillType="$fillType" android:pathData="$data" />""").append('\n')
        }

        // Every use of the logo's definition draws the same fills; its strokes have no width.
        val drawnDefinitions = mutableSetOf<String>()
        fun addShapes(element: Element) {
            when (element.tagName) {
                "title", "desc", "defs" -> {}
                "svg", "g" -> {
                    val children = element.childNodes
                    for (i in 0 until children.length) {
                        (children.item(i) as? Element)?.let(::addShapes)
                    }
                }
                "use" -> {
                    val id = element.getAttribute("href").ifEmpty { element.getAttribute("xlink:href") }.removePrefix("#")
                    if (drawnDefinitions.add(id)) {
                        addShapes(elementsById[id] ?: throw GradleException("Logo uses undefined element '$id'"))
                    }
                }
                "path" -> addPath(element, element.getAttribute("d"))
                "circle" -> {
                    val r = element.getAttribute("r").toDouble()
                    val cx = element.getAttribute("cx").toDouble()
                    val cy = element.getAttribute("cy").toDouble()
                    addPath(element, "M${cx - r},${cy}a$r,$r 0 1,0 ${2 * r},0a$r,$r 0 1,0 ${-2 * r},0Z")
                }
                else -> throw GradleException("Logo contains unsupported element '${element.tagName}'")
            }
        }
        addShapes(svg)

        val target = outputDirectory.get().dir("drawable").file("ic_launcher_logo.xml").asFile
        target.parentFile.mkdirs()
        target.writeText(
            """
            |<?xml version="1.0" encoding="utf-8"?>
            |<vector xmlns:android="http://schemas.android.com/apk/res/android"
            |    android:width="108dp"
            |    android:height="108dp"
            |    android:viewportWidth="$size"
            |    android:viewportHeight="$size">
            |    <group android:translateX="${(size - width) / 2 - x}" android:translateY="${(size - height) / 2 - y}">
            |$paths    </group>
            |</vector>
            |""".trimMargin(),
        )
    }
}

val generateLauncherIcon = tasks.register<GenerateLauncherIcon>("generateLauncherIcon") {
    logo.set(rootProject.layout.projectDirectory.file("../../page/public/sogen.svg"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.res?.addGeneratedSourceDirectory(generateLauncherIcon, GenerateLauncherIcon::outputDirectory)
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

        // Release builds pass the real AdMob app ID with -PadmobAppId=...; the default is Google's sample app.
        manifestPlaceholders["admobAppId"] =
            providers.gradleProperty("admobAppId").getOrElse("ca-app-pub-3940256099942544~3347511713")

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

    // The session runs child processes from libsandbox.so, which only works when the libraries are extracted.
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    // CI writes key.properties when the release key is available. Without it, e.g. in forks and local builds,
    // release builds are signed with the debug key and cannot update an installed official build.
    val keystoreProperties = Properties()
    rootProject.file("key.properties").takeIf { it.exists() }?.inputStream()?.use { keystoreProperties.load(it) }

    signingConfigs {
        if (!keystoreProperties.isEmpty) {
            create("release") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
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
