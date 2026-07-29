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
        versionCode = 510
        versionName = "5.1"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }
    }

    buildTypes {
        // Ugg! The DEBUG apk is the one that ships — the desktop client installs it
        // and CI publishes it. So it gets shrunk like a release build, otherwise the
        // tribe carries 60 MB of unused Compose tooling and icon stones around.
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
        // Ugg! Sources always UTF-8, no matter what fire the build machine sit at.
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

// Ugg! Same UTF-8 rule for every javac fire, also test ones.
tasks.withType<JavaCompile>().configureEach {
    options.encoding = "UTF-8"
}

dependencies {
    // Core & Compose
    // Ugg! Cave man dragged home many tools he never picked up. Every one of them
    // slept inside the shipped apk. Only what the app really touches stays.
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
