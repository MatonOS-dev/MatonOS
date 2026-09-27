package org.matonos.rncommon

import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.camera2.CameraManager
import android.net.wifi.WifiManager
import android.os.BatteryManager
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

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

    val wifiSelected = prop("vendor.maton.wifi.selected")
    val wifiVirtual = wifiSelected.startsWith("hwsim") || wifiSelected.startsWith("virt_wifi")
    val cameras = runCatching {
      (context.getSystemService(Context.CAMERA_SERVICE) as CameraManager).cameraIdList.size
    }.getOrDefault(0)
    val softwareGpu = prop("vendor.mesa.libgl.always.software") == "true"

    return mapOf(
      "battery" to mapOf(
        "present" to batteryPresent,
        "level" to if (batteryPresent && level >= 0 && scale > 0) level * 100 / scale else -1,
        "charging" to (status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL),
        "pluggedIn" to (plugged != 0),
      ),
      "wifi" to mapOf("present" to (prop("vendor.maton.wifi.present") == "1" && !wifiVirtual), "virtual" to wifiVirtual),
      "bluetooth" to mapOf(
        "present" to (prop("vendor.maton.bluetooth.present") == "1"),
        "virtual" to (prop("vendor.maton.bluetooth.virtual") == "1"),
      ),
      "audio" to mapOf("present" to (prop("vendor.maton.audio.selector_result") == "found")),
      "gpu" to mapOf("present" to !softwareGpu, "software" to softwareGpu),
      "camera" to mapOf("count" to cameras),
    )
  }

  /** android.os.SystemProperties is hidden but on the unrestricted (SDK-internal) list. */
  private fun prop(name: String): String = runCatching {
    Class.forName("android.os.SystemProperties").getMethod("get", String::class.java).invoke(null, name) as String
  }.getOrDefault("")
}
