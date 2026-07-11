# Keep PicoVR SDK classes
-keep class com.picovr.** { *; }
-keep class com.psmart.** { *; }
-keep class top.playtbsxys.picostreamer.** { *; }

# Keep native methods
-keepclassmembers class * {
    native <methods>;
}

# Keep VRActivity and related classes
-keep class com.picovr.vractivity.VRActivity { *; }
-keep class com.picovr.vractivity.RenderInterface { *; }
-keep class com.picovr.vractivity.HmdState { *; }
-keep class com.picovr.vractivity.Eye { *; }
-keep class com.picovr.cvclient.** { *; }
