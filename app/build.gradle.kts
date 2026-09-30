plugins {
    id("com.android.application")
}

android {
    // Upgraded from 32 to 33 to expose Vulkan 1.3 native symbols in libvulkan.so
    compileSdk = 33
    ndkVersion = "29.0.14206865"
    namespace = "app.bejzak.bejzak_engine"

    defaultConfig {
        minSdk = 33
        targetSdk = 33
        versionCode = 1
        versionName = "1.0"
        applicationId = "app.bejzak.bejzak_engine"

        externalNativeBuild {
            cmake {
                arguments.add("-DANDROID_STL=c++_shared")
                arguments.add("-DANDROID_USE_LEGACY_TOOLCHAIN_FILE=OFF")
                // Ensure CMake links against the API 33 sysroot libraries
                arguments.add("-DANDROID_PLATFORM=android-33")
            }
            ndk {
                abiFilters.add("arm64-v8a")
            }
        }
    }

    lint {
        disable.add("ExpiredTargetSdkVersion")
    }

    buildTypes {
        getByName("release") {
            isDebuggable = false
            isJniDebuggable = false
            signingConfig = signingConfigs.getByName("debug")
        }
        getByName("debug") {
            isDebuggable = true
            isJniDebuggable = true
        }
    }

    buildFeatures {
        prefab = true
    }

    externalNativeBuild {
        cmake {
            version = "3.22.1"
            path("CMakeLists.txt")
        }
    }

    sourceSets {
        getByName("main") {
            manifest.srcFile("AndroidManifest.xml")
            assets.srcDirs("assets")
        }
        getByName("debug") {
            jniLibs {
                srcDir("libs/debug")
            }
            assets.srcDirs("assets")
        }
        getByName("release") {
            jniLibs.srcDir("libs/release")
        }
    }

    packaging {
        jniLibs {
            keepDebugSymbols.add("**.so")
        }
    }
}

dependencies {
    implementation("org.khronos.openxr:openxr_loader_for_android:1.0.34")
}