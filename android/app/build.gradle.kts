plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "org.megavideoprotect.app"
    compileSdk = 36
    // The SDK image ships no build-tools; the app build script provides a
    // 35.0.0 shim (zipalign/apksigner via apt + source.properties). AGP 8.13
    // refuses anything below its minimum build-tools revision (35.0.0).
    buildToolsVersion = "35.0.0"

    // Release signing. CI decodes the project keystore from the repository
    // secrets and points MVP_KEYSTORE at it. Android only replaces an installed
    // app when the new APK carries the same certificate as the old one, so this
    // key has to stay the same for every release — see android-signing/ next to
    // the repository, not inside it. Without the variables the release build is
    // simply unsigned, which local `assembleDebug` work never notices.
    val keystorePath = System.getenv("MVP_KEYSTORE") ?: ""
    signingConfigs {
        if (keystorePath.isNotBlank()) {
            create("release") {
                storeFile = file(keystorePath)
                // PKCS#12, the format openssl exports and apksigner reads.
                storeType = "PKCS12"
                storePassword = System.getenv("MVP_KEYSTORE_PASSWORD") ?: ""
                keyAlias = System.getenv("MVP_KEY_ALIAS") ?: ""
                keyPassword = System.getenv("MVP_KEY_PASSWORD") ?: ""
            }
        }
    }

    // The SDK image ships NDK 30.0.14904198; AGP's default ndkVersion (27.x)
    // is not installed and cannot be downloaded on this network.
    ndkVersion = "30.0.14904198"

    val mvpVersion = (System.getenv("MVP_VERSION") ?: "").ifBlank { "2.0.3" }
    val mvpMatch = Regex("""(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?""").find(mvpVersion)
        ?: error("MVP_VERSION must look like 2.0.4 or 2.0.4-beta.1, got '$mvpVersion'")
    val mvpMajor = mvpMatch.groupValues[1].toInt()
    val mvpMinor = mvpMatch.groupValues[2].toInt()
    val mvpPatch = mvpMatch.groupValues[3].toInt()
    // A release sits above its own betas and below the next patch.
    val mvpPre = mvpMatch.groupValues[4].toIntOrNull() ?: 99
    val mvpCode = mvpMajor * 10_000_000 + mvpMinor * 100_000 + mvpPatch * 100 + mvpPre

    defaultConfig {
        applicationId = "org.megavideoprotect.app"
        minSdk = 26
        targetSdk = 34
        versionCode = mvpCode
        versionName = mvpVersion
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20")
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DMVP_VERSION=$mvpVersion",
                    "-DMVP_ANDROID_PREFIX=${System.getenv("MVP_ANDROID_PREFIX") ?: "/repo/out/android-arm64/prefix"}"
                )
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // An unsigned release APK cannot update an installed copy, so the
            // workflow hands the keystore in and then checks the signature.
            if (keystorePath.isNotBlank()) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.05.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.0")
    // Real playback (hardware-decoded video + audio) straight from the vault:
    // ExoPlayer reads the decrypted stream through VaultDataSource (JNI).
    implementation("androidx.media3:media3-exoplayer:1.6.1")
    implementation("androidx.media3:media3-ui:1.6.1")
    // Login-screen scanner for the PC share code.
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")
    // Frosted bars: the library draws behind the header and footer.
    implementation("dev.chrisbanes.haze:haze:1.6.1")
}
