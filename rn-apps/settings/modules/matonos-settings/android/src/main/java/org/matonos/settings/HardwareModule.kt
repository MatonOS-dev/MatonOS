package org.matonos.settings.nativebridge

import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.camera2.CameraManager
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.provider.Settings
import android.util.Log
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import org.matonos.settings.SystemComponents
import org.matonos.settings.EmbeddedSettingsDiscovery

/**
 * What hardware is REALLY present, so UIs can hide icons for missing hardware
 * (battery on desktops, no Wi-Fi adapter, ...). Wi-Fi and Bluetooth are spoofed
 * by MatonOS when absent (Android's own feature checks always say yes); the real
 * state comes from the MatonOS daemons' vendor.maton.<area>.* properties.
 */
class HardwareModule : Module() {
  private var receiver: BroadcastReceiver? = null

  override fun definition() = ModuleDefinition {
    Name("MatonHardware")
    Events("hardwareChanged")

    AsyncFunction("getHardwareState") { state(requireNotNull(appContext.reactContext)) }
    AsyncFunction("getSecurityStatus") { securityStatus(requireNotNull(appContext.reactContext)) }
    AsyncFunction("getEmbeddedSettings") {
      EmbeddedSettingsDiscovery.list(requireNotNull(appContext.reactContext))
    }
    AsyncFunction("getSystemComponents") {
      SystemComponents.read(requireNotNull(appContext.reactContext))
    }
    AsyncFunction("setSystemComponent") { kind: String, flattened: String ->
      SystemComponents.write(requireNotNull(appContext.reactContext), kind, flattened)
    }

    OnStartObserving {
      val context = requireNotNull(appContext.reactContext)
      val r = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) = sendEvent("hardwareChanged", state(c))
      }
      receiver = r
      val filter = IntentFilter().apply {
        addAction(Intent.ACTION_BATTERY_CHANGED)
        addAction(Intent.ACTION_POWER_CONNECTED)
        addAction(Intent.ACTION_POWER_DISCONNECTED)
        addAction(WifiManager.WIFI_STATE_CHANGED_ACTION)
        addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
      }
      context.registerReceiver(r, filter, Context.RECEIVER_EXPORTED)
    }
    OnStopObserving {
      receiver?.let { r -> runCatching { appContext.reactContext?.unregisterReceiver(r) } }
      receiver = null
    }
  }

  private fun state(context: Context): Map<String, Any> {
    val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    val batteryPresent = battery?.getBooleanExtra(BatteryManager.EXTRA_PRESENT, false) ?: false
    val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
    val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
    val status = battery?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
    val plugged = battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0

    val cameras = runCatching {
      (context.getSystemService(Context.CAMERA_SERVICE) as CameraManager).cameraIdList.size
    }.getOrDefault(0)

    return mapOf(
      "battery" to mapOf(
        "present" to batteryPresent,
        "level" to if (batteryPresent && level >= 0 && scale > 0) level * 100 / scale else -1,
        "charging" to (status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL),
        "pluggedIn" to (plugged != 0),
      ),
      // Daemon-detected hardware: Hardware.ts fills these from the System Bridge
      // "hardware" summary; null here means unknown.
      "wifi" to mapOf("present" to null, "virtual" to null),
      "bluetooth" to mapOf("present" to null, "virtual" to null),
      "audio" to mapOf("present" to null),
      "gpu" to mapOf("present" to null, "software" to null),
      "camera" to mapOf("count" to cameras),
    )
  }

  /** Public-API security values only. SELinux, encryption, vendor patch level and verified
   * boot need platform-signed property access and come from the System Bridge (Security.ts). */
  private fun securityStatus(context: Context): Map<String, String> {
    val adb = try {
      if (Settings.Global.getInt(context.contentResolver, Settings.Global.ADB_ENABLED, 0) == 1) "Enabled" else "Disabled"
    } catch (e: Exception) {
      Log.e("MatonOSSettings", "security status read failed: Settings.Global.ADB_ENABLED", e)
      "Unknown"
    }
    val patch = try {
      Build.VERSION.SECURITY_PATCH.takeIf { it.isNotBlank() } ?: "Unknown".also {
        Log.e("MatonOSSettings", "security status unavailable: Build.VERSION.SECURITY_PATCH is empty")
      }
    } catch (e: Exception) {
      Log.e("MatonOSSettings", "security status read failed: Build.VERSION.SECURITY_PATCH", e)
      "Unknown"
    }
    val tags = try {
      Build.TAGS?.takeIf { it.isNotBlank() } ?: "Unknown".also {
        Log.e("MatonOSSettings", "security status unavailable: Build.TAGS is empty")
      }
    } catch (e: Exception) {
      Log.e("MatonOSSettings", "security status read failed: Build.TAGS", e)
      "Unknown"
    }
    return mapOf(
      "securityPatch" to patch,
      "buildKeys" to when {
        tags.contains("release-keys") -> "Release keys"
        // dev-keys = AOSP's tag for private non-test keys (MatonOS release keys, unsigned target-files).
        tags.contains("dev-keys") -> "Private keys"
        tags.contains("test-keys") -> "Public test keys"
        tags == "Unknown" -> tags
        else -> tags
      },
      "adb" to adb,
    )
  }

}
