package org.matonos.settings.nativebridge

import expo.modules.kotlin.Promise
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import android.app.WallpaperManager
import android.content.Intent
import android.os.Build
import org.matonos.client.BridgeMode
import org.matonos.client.MatonOS
import org.matonos.client.MatonosClient
import org.matonos.settings.OtaInstallerBridge
import org.matonos.settings.AccountFlowActivity
import org.matonos.settings.gapps.GappsInstaller
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
    AsyncFunction("stageUpdate") { names: List<String>, uris: List<String> ->
      OtaInstallerBridge.stage(requireNotNull(appContext.reactContext), requireClient(), names, uris)
    }
    AsyncFunction("finalizeUpdate") {
      val result = requireClient().finalizeUpdate()
      mapOf("available" to result.available, "value" to (result.value ?: ""), "reason" to (result.reason ?: ""))
    }
    AsyncFunction("getUpdateStatus") {
      val result = requireClient().getUpdateStatus()
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
    AsyncFunction("pickAndInstallGapps") { promise: Promise ->
      val activity = appContext.currentActivity
      if (activity == null) promise.resolve(mapOf("ok" to false, "message" to "Settings activity is unavailable."))
      else GappsInstaller.pick(activity, promise)
    }
    AsyncFunction("uninstallGappsUpdates") { promise: Promise ->
      val context = requireNotNull(appContext.reactContext)
      GappsInstaller.uninstallUpdates(context, promise)
    }
    AsyncFunction("getGappsDeviceId") {
      GappsInstaller.deviceId(requireNotNull(appContext.reactContext))
    }
    AsyncFunction("getSystemInfo") {
      mapOf("version" to Build.VERSION.RELEASE, "build" to Build.DISPLAY,
        "device" to Build.DEVICE, "model" to Build.MODEL, "sdk" to Build.VERSION.SDK_INT)
    }
    AsyncFunction("accountMachineInfo") {
      val context = requireNotNull(appContext.reactContext)
      val machineId = android.provider.Settings.Secure.getString(context.contentResolver, android.provider.Settings.Secure.ANDROID_ID)
        ?: throw IllegalStateException("This device has no Android ID")
      val deviceName = android.provider.Settings.Global.getString(context.contentResolver, android.provider.Settings.Global.DEVICE_NAME)
        ?.takeIf { it.isNotBlank() } ?: Build.MODEL
      mapOf("machineId" to machineId, "deviceName" to deviceName)
    }
    AsyncFunction("accountComplete") { username: String, email: String, token: String, refreshToken: String, machineId: String ->
      require(username.isNotBlank() && token.isNotBlank()) { "Account name and session token are required" }
      val context = requireNotNull(appContext.reactContext)
      val manager = android.accounts.AccountManager.get(context)
      val account = android.accounts.Account(username, "org.matonos.account")
      if (manager.getAccountsByType("org.matonos.account").isEmpty()) {
        if (!manager.addAccountExplicitly(account, null, android.os.Bundle())) throw IllegalStateException("Could not add MatonOS account")
      } else if (manager.getAccountsByType("org.matonos.account").none { it.name.equals(username, ignoreCase = true) }) {
        throw IllegalStateException("Only one MatonOS account can be added")
      }
      manager.setUserData(account, "email", email)
      manager.setUserData(account, "machine_id", machineId)
      manager.setAuthToken(account, "session", token)
      manager.setPassword(account, refreshToken)
      val result = Intent().putExtra(android.accounts.AccountManager.KEY_ACCOUNT_NAME, username)
        .putExtra(android.accounts.AccountManager.KEY_ACCOUNT_TYPE, "org.matonos.account")
        .putExtra(android.accounts.AccountManager.KEY_AUTHTOKEN, token)
        .putExtra("refresh_token", refreshToken)
        .putExtra(org.matonos.settings.nativebridge.AccountAuthenticator.EXTRA_MACHINE_ID, machineId)
      if (AccountFlowActivity.hasPendingFlow()) appContext.currentActivity?.let { activity ->
        activity.setResult(android.app.Activity.RESULT_OK, result)
        activity.finish()
      }
      true
    }
    AsyncFunction("accountInvalidate") { token: String ->
      android.accounts.AccountManager.get(requireNotNull(appContext.reactContext)).invalidateAuthToken("org.matonos.account", token)
      true
    }
    AsyncFunction("accountStoredSession") {
      val context = requireNotNull(appContext.reactContext)
      val manager = android.accounts.AccountManager.get(context)
      val account = manager.getAccountsByType("org.matonos.account").firstOrNull() ?: return@AsyncFunction null
      var token = manager.peekAuthToken(account, "session") ?: return@AsyncFunction null
      val machineId = manager.getUserData(account, "machine_id") ?: return@AsyncFunction null
      if (org.matonos.settings.nativebridge.AccountAuthenticator(context).tokenExpiry(token) <= System.currentTimeMillis() / 1000 + 300) {
        val refresh = manager.getPassword(account)
        if (refresh.isNullOrBlank()) return@AsyncFunction null
        try {
          val renewed = org.matonos.settings.nativebridge.AccountAuthenticator(context).refreshCredentials(refresh, machineId)
          token = renewed.first
          manager.setAuthToken(account, "session", renewed.first)
          manager.setPassword(account, renewed.second)
        } catch (e: org.matonos.settings.nativebridge.AccountAuthenticator.RefreshFailure) {
          if (!e.unauthorized) throw IllegalStateException(e.message ?: "MatonOS session refresh failed")
          manager.invalidateAuthToken("org.matonos.account", token)
          manager.setPassword(account, null)
          return@AsyncFunction null
        }
      }
      mapOf("username" to account.name, "email" to (manager.getUserData(account, "email") ?: ""), "token" to token, "refreshToken" to (manager.getPassword(account) ?: ""))
    }
    AsyncFunction("accountRemoveLocal") { username: String ->
      val manager = android.accounts.AccountManager.get(requireNotNull(appContext.reactContext))
      val account = manager.getAccountsByType("org.matonos.account").firstOrNull { it.name == username }
      if (account != null) manager.removeAccountExplicitly(account)
      true
    }
    AsyncFunction("accountCancel") {
      if (AccountFlowActivity.hasPendingFlow()) appContext.currentActivity?.let { activity ->
        activity.setResult(android.app.Activity.RESULT_CANCELED)
        activity.finish()
      }
      true
    }
    AsyncFunction("openTrustedApps") {
      val context = requireNotNull(appContext.reactContext)
      val intent = Intent("org.matonos.systembridge.TRUST_SETTINGS").setPackage("org.matonos.systembridge")
      intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
      context.startActivity(intent)
      true
    }
    AsyncFunction("reboot") {
      val context = requireNotNull(appContext.reactContext)
      context.getSystemService(android.os.PowerManager::class.java).reboot(null)
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

  private fun snapshot(current: MatonosClient, available: Boolean, reason: String?, channels: List<String>) = mapOf(
    "available" to available,
    "reason" to (reason ?: ""),
    "apiVersion" to current.bridgeApiVersion,
    "accessAllowed" to available,
    "missingChannels" to if (reason?.startsWith("CHANNEL_UNAVAILABLE:") == true) listOf(reason.substringAfter(':')) else emptyList<String>(),
  )
}
