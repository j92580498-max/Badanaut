plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val noticeAssetRoot = layout.buildDirectory.dir("generated/noticeAssets")
val prepareNotices by tasks.registering(Copy::class) {
    val repositoryRoot = rootProject.projectDir.resolve("../..")
    val noticePaths = repositoryRoot.resolve("third_party/notices.txt").readLines()
    from(repositoryRoot) {
        include(noticePaths)
    }
    into(noticeAssetRoot.map { it.dir("notices") })
    doFirst {
        noticePaths.forEach { path ->
            check(repositoryRoot.resolve(path).isFile) {
                "Missing distribution notice: $path; initialize Git submodules first"
            }
        }
    }
}

tasks.named("preBuild") {
    dependsOn(prepareNotices)
}

android {
    namespace = "org.badanaut.emulator"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "org.badanaut.emulator"
        minSdk = 24
        targetSdk = 34
        versionCode = 12
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets.getByName("main") {
        assets.srcDir("../../shared-ui")
        assets.srcDir(noticeAssetRoot)
        java.srcDir("../../../third_party/SDL2/android-project/app/src/main/java")
        // Native libraries are built by CMake; ignore generated files from older builds.
        jniLibs.setSrcDirs(emptyList<String>())
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    testOptions {
        unitTests.isIncludeAndroidResources = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.0")
    implementation("androidx.fragment:fragment-ktx:1.7.1")
    implementation("com.google.android.material:material:1.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.0")
    implementation("org.json:json:20240303")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
}
