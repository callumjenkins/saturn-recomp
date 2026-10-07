// A game's playtest app, built by the release workflow (playtest-release.yml) with:
//   -Pplaytest.bundle=DIR    playtest.json and disc.json, from `python -m saturnrecomp.playtest GAME.toml bundle`
//   -Pplaytest.native=DIR    arm64-v8a/ holding libmain.so (the game) and libSDL3.so
//   -Psdl.java=DIR           SDL's Java sources, from the same SDL as libSDL3.so (android-project/app/src/main/java)
//   -Pplaytest.id=ID         the application id: one per game, so two games install side by side
//   -Pplaytest.name=NAME     the game's name, under the app's icon
//   -Pplaytest.build=BUILD   the build id
//   -Pplaytest.debuggable    optional: lets `adb shell run-as` read the app's files, for a maintainer's own phone
// assembleRelease leaves app-release-unsigned.apk, which the workflow signs with apksigner.
plugins {
    id("com.android.application")
}

fun prop(name: String): String = providers.gradleProperty(name).orNull ?: error("-P$name is required")

android {
    namespace = "org.saturnrecomp.playtest"
    compileSdk = 36
    defaultConfig {
        applicationId = prop("playtest.id")
        minSdk = 26
        targetSdk = 36
        // Android installs over an earlier build only one with a higher code: minutes since 1970
        versionCode = (System.currentTimeMillis() / 60000).toInt()
        versionName = prop("playtest.build")
        manifestPlaceholders["gameName"] = prop("playtest.name")
        ndk { abiFilters += "arm64-v8a" }
    }
    sourceSets["main"].apply {
        java.srcDir(prop("sdl.java"))
        assets.srcDir(prop("playtest.bundle"))
        jniLibs.srcDir(prop("playtest.native"))
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            isDebuggable = providers.gradleProperty("playtest.debuggable").isPresent
        }
    }
    packaging { jniLibs.keepDebugSymbols += "**/*.so" }     // the workflow strips them
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20250517")            // Android's own org.json is a stub off the device
}
