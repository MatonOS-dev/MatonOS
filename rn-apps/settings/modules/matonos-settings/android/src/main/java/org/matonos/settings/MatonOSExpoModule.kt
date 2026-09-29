package org.matonos.settings.nativebridge

import expo.modules.kotlin.Promise
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import android.app.WallpaperManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.graphics.Bitmap
import android.graphics.Canvas
import android.net.Uri
import org.matonos.client.BridgeMode
import org.matonos.client.MatonOS
import org.matonos.client.MatonosClient
import org.json.JSONArray
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.ConcurrentHashMap

/** Expo adapter over buildinfra/client. All bridge semantics remain in that shared Java client. */
class MatonOSExpoModule : Module() {
  private var client: MatonosClient? = null
  private val listeners = ConcurrentHashMap<String, MatonosClient.EventListener>()
  private var availabilityListener: MatonosClient.AvailabilityListener? = null

  override fun definition() = ModuleDefinition {
    Name("MatonOSClient")
    Events("matonosAreaEvent", "matonosAvailabilityChanged")

    OnCreate {
      val context = requireNotNull(appContext.reactContext)
      val current = MatonOS.init(context, BridgeMode.OPTIONAL)
      client = current
      val callback = MatonosClient.AvailabilityListener { available, reason ->
        sendEvent("matonosAvailabilityChanged", snapshot(current, available, reason, emptyList()))
      }
      availabilityListener = callback
      current.addAvailabilityListener(callback)
    }
    OnDestroy {
      client?.let { current ->
        availabilityListener?.let(current::removeAvailabilityListener)
        listeners.forEach { (id, listener) ->
          val parts = id.split(":", limit = 3)
          if (parts.size == 3) current.unsubscribe(parts[0], parts[1], listener)
        }
        current.close()
      }
      listeners.clear()
      client = null
    }

    AsyncFunction("checkStartup") { accessTargets: List<String>, requiredChannels: List<String>, promise: Promise ->
      val current = requireClient()
      current.checkStartup(accessTargets.toTypedArray(), requiredChannels.toTypedArray()) { result ->
        promise.resolve(snapshot(current, result.available, result.reason, requiredChannels))
      }
    }
    AsyncFunction("getBridgeSnapshot") { accessTarget: String ->
      val current = requireClient()
      mapOf(
        "available" to current.isAvailable(), "reason" to (current.reason ?: ""),
        "apiVersion" to current.bridgeApiVersion, "accessAllowed" to current.isTargetAllowed(accessTarget),
        "missingChannels" to emptyList<String>(),
      )
    }
    AsyncFunction("call") { target: String, command: String, jsonArgs: String ->
      val result = requireClient().call(target, command, jsonArgs)
      mapOf("available" to result.available, "value" to (result.value ?: ""), "reason" to (result.reason ?: ""))
    }
    AsyncFunction("isLiveImage") {
      try {
        val process = ProcessBuilder("/system/bin/getprop", "ro.boot.matonos.live").redirectErrorStream(true).start()
        if (!process.waitFor(1200, java.util.concurrent.TimeUnit.MILLISECONDS)) {
          process.destroyForcibly(); false
        } else process.exitValue() == 0 && process.inputStream.bufferedReader().use { it.readText().trim() } == "1"
      } catch (_: Exception) { false }
    }
    AsyncFunction("getSystemInfo") {
      mapOf("version" to Build.VERSION.RELEASE, "build" to Build.DISPLAY,
        "device" to Build.DEVICE, "model" to Build.MODEL, "sdk" to Build.VERSION.SDK_INT)
    }
    AsyncFunction("openTrustedApps") {
      val context = requireNotNull(appContext.reactContext)
      val intent = Intent("org.matonos.systembridge.TRUST_SETTINGS").setPackage("org.matonos.systembridge")
      intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
      context.startActivity(intent)
      true
    }
    AsyncFunction("finishActivity") {
      appContext.currentActivity?.finish()
      true
    }
    AsyncFunction("getWallpaperSeedColor") {
      try {
        WallpaperManager.getInstance(requireNotNull(appContext.reactContext))
          .getWallpaperColors(WallpaperManager.FLAG_SYSTEM)?.primaryColor?.toArgb() ?: 0xff6750a4.toInt()
      } catch (_: Exception) { 0xff6750a4.toInt() }
    }
    AsyncFunction("navigate") { action: String, longPress: Boolean ->
      val method = when (action) {
        "back" -> "navigateBack"
        "home" -> "navigateHome"
        "recents" -> "navigateRecents"
        else -> return@AsyncFunction resultMap(false, false, "INVALID_NAVIGATION_ACTION")
      }
      val current = requireClient()
      try {
        // The stable Java client gains these methods with the matching bridge
        // update. Reflection keeps this package source-compatible while an
        // older image reports an unavailable capability cleanly.
        val result = if (action == "back") current.javaClass.getMethod(method, Boolean::class.javaPrimitiveType!!).invoke(current, longPress)
          else current.javaClass.getMethod(method).invoke(current)
        val available = result.javaClass.getField("available").getBoolean(result)
        val value = result.javaClass.getField("value").get(result) as? Boolean ?: false
        val reason = result.javaClass.getField("reason").get(result) as? String
        resultMap(available, value, reason)
      } catch (_: ReflectiveOperationException) {
        resultMap(false, false, "NAVIGATION_CAPABILITY_UNAVAILABLE")
      }
    }
    AsyncFunction("getRecentTasks") { maxTasks: Int ->
      val result = requireClient().getRecentTasks(maxTasks.coerceIn(1, 100))
      val enriched = if (result.available && result.value != null) enrichRecentTasks(
        requireNotNull(appContext.reactContext), result.value,
      ) else result.value
      resultMap(result.available, enriched, result.reason)
    }
    AsyncFunction("moveTaskToFront") { taskId: Int ->
      invokeBooleanClientTask("moveTaskToFront", taskId)
    }
    AsyncFunction("setTaskFullscreen") { taskId: Int ->
      invokeBooleanClientTask("setTaskFullscreen", taskId)
    }
    AsyncFunction("removeRecentTask") { taskId: Int ->
      val result = requireClient().removeRecentTask(taskId)
      resultMap(result.available, result.value, result.reason)
    }
    AsyncFunction("getRecentTaskThumbnail") { taskId: Int ->
      val current = requireClient()
      try {
        val result = current.javaClass
          .getMethod("getRecentTaskThumbnail", Int::class.javaPrimitiveType!!)
          .invoke(current, taskId)
        val available = result.javaClass.getField("available").getBoolean(result)
        val bytes = result.javaClass.getField("value").get(result) as? ByteArray
        val reason = result.javaClass.getField("reason").get(result) as? String
        if (!available || bytes == null) resultMap(false, "", reason)
        else resultMap(true, cacheThumbnail(requireNotNull(appContext.reactContext), taskId, bytes), null)
      } catch (_: ReflectiveOperationException) {
        resultMap(false, "", "THUMBNAIL_CAPABILITY_UNAVAILABLE")
      }
    }
    AsyncFunction("subscribe") { target: String, topic: String, subscriptionId: String ->
      val current = requireClient()
      if (listeners.containsKey(subscriptionId)) return@AsyncFunction true
      val listener = MatonosClient.EventListener { eventTarget, eventTopic, json ->
        sendEvent("matonosAreaEvent", mapOf("target" to eventTarget, "topic" to eventTopic, "json" to json))
      }
      val result = current.subscribe(target, topic, listener)
      if (result.available) listeners[subscriptionId] = listener
      result.available
    }
    AsyncFunction("unsubscribe") { subscriptionId: String ->
      val listener = listeners.remove(subscriptionId) ?: return@AsyncFunction true
      val parts = subscriptionId.split(":", limit = 3)
      if (parts.size != 3) return@AsyncFunction false
      requireClient().unsubscribe(parts[0], parts[1], listener).available
    }
  }

  private fun requireClient() = requireNotNull(client) { "MatonOS client is not initialized" }

  private fun invokeBooleanClientTask(methodName: String, taskId: Int): Map<String, Any?> {
    val current = requireClient()
    return try {
      val result = current.javaClass
        .getMethod(methodName, Int::class.javaPrimitiveType!!)
        .invoke(current, taskId)
      val available = result.javaClass.getField("available").getBoolean(result)
      val value = result.javaClass.getField("value").get(result) as? Boolean ?: false
      val reason = result.javaClass.getField("reason").get(result) as? String
      resultMap(available, value, reason)
    } catch (_: NoSuchMethodException) {
      resultMap(false, false, "${methodName.uppercase()}_CLIENT_API_UNAVAILABLE")
    } catch (error: ReflectiveOperationException) {
      resultMap(false, false, error.message)
    }
  }

  private fun enrichRecentTasks(context: Context, json: String): String {
    return try {
      val tasks = JSONArray(json)
      for (index in 0 until tasks.length()) {
        val task = tasks.optJSONObject(index) ?: continue
        val packageName = task.optString("packageName")
        if (packageName.isBlank()) continue
        try {
          val application = context.packageManager.getApplicationInfo(packageName, 0)
          task.put("label", context.packageManager.getApplicationLabel(application).toString())
          task.put("iconUri", cacheTaskIcon(context, packageName))
        } catch (_: Exception) {
          task.put("label", packageName)
        }
        if (!task.has("thumbnailUri")) task.put("thumbnailUri", "")
      }
      tasks.toString()
    } catch (_: Exception) { json }
  }

  private fun cacheTaskIcon(context: Context, packageName: String): String {
    val name = packageName.replace(Regex("[^A-Za-z0-9._-]"), "_")
    val directory = File(context.cacheDir, "recent-task-icons")
    val file = File(directory, "$name.png")
    if (file.isFile) return Uri.fromFile(file).toString()
    return try {
      directory.mkdirs()
      val size = (96 * context.resources.displayMetrics.density).toInt().coerceAtLeast(64)
      val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
      val icon = context.packageManager.getApplicationIcon(packageName)
      icon.setBounds(0, 0, size, size)
      icon.draw(Canvas(bitmap))
      FileOutputStream(file).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
      bitmap.recycle()
      Uri.fromFile(file).toString()
    } catch (_: Exception) { "" }
  }

  private fun cacheThumbnail(context: Context, taskId: Int, bytes: ByteArray): String {
    if (bytes.isEmpty() || bytes.size > 2 * 1024 * 1024) return ""
    return try {
      val directory = File(context.cacheDir, "recent-task-thumbnails")
      directory.mkdirs()
      val file = File(directory, "$taskId.png")
      FileOutputStream(file).use { it.write(bytes) }
      Uri.fromFile(file).toString()
    } catch (_: Exception) { "" }
  }

  private fun resultMap(available: Boolean, value: Any?, reason: String?) = mapOf(
    "available" to available,
    "value" to (value?.toString() ?: ""),
    "reason" to (reason ?: ""),
  )

  private fun snapshot(current: MatonosClient, available: Boolean, reason: String?, channels: List<String>) = mapOf(
    "available" to available,
    "reason" to (reason ?: ""),
    "apiVersion" to current.bridgeApiVersion,
    "accessAllowed" to available,
    "missingChannels" to if (reason?.startsWith("CHANNEL_UNAVAILABLE:") == true) listOf(reason.substringAfter(':')) else emptyList<String>(),
  )
}
