# Ugg! R8 throws away every stone nobody points at. Some stones are only named in
# the manifest or picked up by name at runtime, so cave man marks them by hand.

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

# Cave man wants readable crash stones from the field.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
