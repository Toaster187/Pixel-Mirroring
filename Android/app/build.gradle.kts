plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
}

android {
    namespace = "dev.pixelmirroring.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.pixelmirroring.app"
        minSdk = 30 // Android 11 for Wireless Debugging support
        targetSdk = 35
        versionCode = 520
        versionName = "5.2"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }
    }

    buildTypes {
        // The DEBUG apk is the one that ships — the desktop client installs it and CI
        // publishes it. So it is shrunk like a release build; without that it is ~60 MB
        // of unused Compose tooling and icons instead of ~4 MB.
        debug {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
        // Sources are always UTF-8, whatever the build machine's default encoding is.
        encoding = "UTF-8"
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

// The same UTF-8 rule for every javac task, including the test ones.
tasks.withType<JavaCompile>().configureEach {
    options.encoding = "UTF-8"
}

dependencies {
    // Core & Compose
    // Only dependencies the app actually uses. Unused ones still end up in the
    // shipped apk, and apk size is a feature here.
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.7.0")
    implementation("androidx.activity:activity-compose:1.8.2")
    implementation(platform("androidx.compose:compose-bom:2024.01.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.material3:material3")
    // NOTE: no material-icons-extended — the whole app uses exactly one icon
    // (Icons.Filled.CheckCircle), and that one lives in material-icons-core,
    // which material3 already brings along.

    // DataStore
    implementation("androidx.datastore:datastore-preferences:1.0.0")

    // JSON for tiny local discovery server
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.5.1")

    // Coroutines
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.11.1")
    testImplementation("androidx.test:core:1.5.0")
    testImplementation("androidx.test.ext:junit:1.1.5")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.7.3")
    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
    androidTestImplementation(platform("androidx.compose:compose-bom:2024.01.00"))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    // Compose tooling used to be a debugImplementation, but the debug apk is what
    // users install — the preview/inspection tooling was pure ballast in there.
    // Turn it back on locally with: gradle assembleDebug -PcomposeTooling
    if (project.hasProperty("composeTooling")) {
        debugImplementation("androidx.compose.ui:ui-tooling")
        debugImplementation("androidx.compose.ui:ui-test-manifest")
    }
}
