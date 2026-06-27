package com.example.apxdb

import androidx.annotation.NonNull
import io.flutter.embedding.engine.plugins.FlutterPlugin

class ApxdbPlugin: FlutterPlugin {
    override fun onAttachedToEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {
        // Native FFI load happens from Dart side.
    }

    override fun onDetachedFromEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {
        // Clean up plugin-level state if needed.
    }
}
