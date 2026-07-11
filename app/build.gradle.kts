plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "top.playtbsxys.picostreamer"
    compileSdk = 36  // Required by AGP 9

    defaultConfig {
        applicationId = "top.playtbsxys.picostreamer"
        minSdk = 27          // Pico Neo 2 = Android 8.1 (API 27)
        targetSdk = 27       // Pico Neo 2 = Android 8.1 (API 27)
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
                cFlags += "-Wno-unused-parameter"
                cppFlags += listOf("-std=c++17", "-fexceptions", "-frtti")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            // ALVR client_core shared libraries built by cargo xtask
            val alvrBuildDir = file("${rootDir}/../ALVR/build/alvr_client_core")
            jniLibs.srcDirs(alvrBuildDir)
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    // PicoVR Native SDK
    implementation(files("../../picosdk/PvrSDK-Native-release.aar"))
    implementation("com.android.support:support-annotations:28.0.0")
}

// Task to build ALVR client_core library via cargo xtask
// Set ANDROID_NDK_HOME environment variable before building, or cargo will use its own NDK discovery.
tasks.register<Exec>("buildAlvrClientLib") {
    workingDir = file("${rootDir}/../ALVR")
    System.getenv("ANDROID_NDK_HOME")?.let {
        environment("ANDROID_NDK_HOME", it)
    }
    commandLine(
        "cargo", "run", "--package", "alvr_xtask", "--",
        "build-client-lib", "--no-stdcpp", "--release"
    )
}

tasks.named("preBuild") {
    dependsOn("buildAlvrClientLib")
}
