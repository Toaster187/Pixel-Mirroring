# R8 removes everything nothing references. Classes that are only named in the
# manifest or looked up reflectively at runtime have to be kept by hand.

# Components named in AndroidManifest.xml. AGP keeps these on its own, but being
# explicit means a rename in the manifest can never silently kill them.
-keep class dev.pixelmirroring.app.MainActivity { *; }
-keep class dev.pixelmirroring.app.service.MirroringService { *; }
-keep class dev.pixelmirroring.app.service.BootReceiver { *; }
-keep class dev.pixelmirroring.app.service.MediaScannerReceiver { *; }

# Wire models talked over the discovery HTTP server. kotlinx-serialization ships its
# own rules, but the generated $$serializer companions must survive for the PC to
# still understand the phone.
-keepclassmembers @kotlinx.serialization.Serializable class dev.pixelmirroring.app.network.** {
    *** Companion;
    *** INSTANCE;
    kotlinx.serialization.KSerializer serializer(...);
}
-keep,includedescriptorclasses class dev.pixelmirroring.app.network.**$$serializer { *; }

# Keep line numbers so crash reports from the field stay readable.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
