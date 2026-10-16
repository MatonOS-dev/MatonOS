package org.matonos.flathub.nativebridge

import expo.modules.kotlin.Promise
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import org.matonos.client.BridgeMode
import org.matonos.client.MatonOS
import org.matonos.client.MatonosClient
import org.matonos.linux.ILinuxAppStore
import org.matonos.linux.ILinuxAppStoreCallback
import java.util.concurrent.ConcurrentHashMap

class MatonOSExpoModule : Module() {
  private var client: MatonosClient? = null
  private val listeners = ConcurrentHashMap<String, MatonosClient.EventListener>()
  private val storeConnections = ConcurrentHashMap.newKeySet<android.content.ServiceConnection>()

  override fun definition() = ModuleDefinition {
    Name("MatonOSClient")
    Events("matonosAreaEvent", "linuxAppStoreEvent")
    OnCreate {
      client = MatonOS.init(requireNotNull(appContext.reactContext), BridgeMode.OPTIONAL)
    }
    OnDestroy {
      val context = appContext.reactContext
      if (context != null) storeConnections.forEach { try { context.unbindService(it) } catch (_: Exception) { } }
      storeConnections.clear()
      client?.let { current ->
        listeners.forEach { (id, listener) ->
          val parts = id.split(":", limit = 3)
          if (parts.size == 3) current.unsubscribe(parts[0], parts[1], listener)
        }
        current.close()
      }
      listeners.clear()
      client = null
    }
    AsyncFunction("checkStartup") { targets: List<String>, channels: List<String>, promise: Promise ->
      val current = requireClient()
      current.checkStartup(targets.toTypedArray(), channels.toTypedArray()) { result ->
        promise.resolve(mapOf("available" to result.available, "reason" to (result.reason ?: ""),
          "apiVersion" to current.bridgeApiVersion, "accessAllowed" to result.available,
          "missingChannels" to if (result.reason?.startsWith("CHANNEL_UNAVAILABLE:") == true) listOf(result.reason.substringAfter(':')) else emptyList<String>()))
      }
    }
    AsyncFunction("call") { target: String, command: String, args: String ->
      val result = requireClient().call(target, command, args)
      mapOf("available" to result.available, "value" to (result.value ?: ""), "reason" to (result.reason ?: ""))
    }
    // "Install me" / "update me": wait for the stub, then start its activity
    // (found by its Flatpak ref metadata). The stub calls the subsystem store itself;
    // "install me" does nothing if the Flatpak is installed, so it also resumes.
    AsyncFunction("startStubInstall") { ref: String, operationId: String? ->
      startStubActivity(STUB_INSTALL_ACTION, ref, operationId)
    }
    AsyncFunction("startStubUpdate") { ref: String, operationId: String? ->
      startStubActivity(STUB_UPDATE_ACTION, ref, operationId)
    }
    AsyncFunction("launchStub") { appId: String -> launchStub(appId) }
    AsyncFunction("subscribe") { target: String, topic: String, id: String ->
      if (listeners.containsKey(id)) return@AsyncFunction true
      val callback = MatonosClient.EventListener { eventTarget, eventTopic, json ->
        sendEvent("matonosAreaEvent", mapOf("target" to eventTarget, "topic" to eventTopic, "json" to json))
      }
      val result = requireClient().subscribe(target, topic, callback)
      if (result.available) listeners[id] = callback
      result.available
    }
    AsyncFunction("unsubscribe") { id: String ->
      val callback = listeners.remove(id) ?: return@AsyncFunction true
      val parts = id.split(":", limit = 3)
      if (parts.size != 3) return@AsyncFunction false
      requireClient().unsubscribe(parts[0], parts[1], callback).available
    }
    AsyncFunction("storeInstall") { ref: String, operationId: String, promise: Promise ->
      withStore(promise) { store ->
        store.addRemote("flathub", FLATHUB_URL, "${operationId}_remote", object : Callback() {
          override fun onResult(id: String, ok: Boolean, error: String?) {
            if (!ok && error?.contains("already exists", ignoreCase = true) != true) {
              emitResult(operationId, false, error)
              promise.reject("STORE_INSTALL", error ?: "Could not configure Flathub", null)
              return
            }
            store.install(ref, "flathub", operationId, callbackFor())
            promise.resolve(true)
          }
        })
      }
    }
    AsyncFunction("storeUpdate") { ref: String, operationId: String, promise: Promise ->
      withStore(promise) { it.update(ref, operationId, callbackFor()); promise.resolve(true) }
    }
    AsyncFunction("storeUninstall") { ref: String, operationId: String, promise: Promise ->
      withStore(promise) { it.uninstall(ref, operationId, callbackFor()); promise.resolve(true) }
    }
    AsyncFunction("storeListInstalled") { promise: Promise -> withStore(promise) { promise.resolve(it.listInstalled()) } }
    AsyncFunction("storeListRemotes") { promise: Promise -> withStore(promise) { promise.resolve(it.listRemotes()) } }
  }

  private abstract inner class Callback : ILinuxAppStoreCallback.Stub() {
    override fun onProgress(id: String, phase: String, percent: Int, bytesDone: Long, bytesTotal: Long) {
      sendEvent("linuxAppStoreEvent", mapOf("operationId" to id, "phase" to phase, "percent" to percent,
        "bytesDone" to bytesDone, "bytesTotal" to bytesTotal))
    }
    open override fun onResult(id: String, ok: Boolean, error: String?) { emitResult(id, ok, error) }
  }

  private fun callbackFor() = object : Callback() {
    override fun onResult(operationId: String, ok: Boolean, error: String?) {
      emitResult(operationId, ok, error)
    }
  }

  private fun emitResult(id: String, ok: Boolean, error: String?) {
    sendEvent("linuxAppStoreEvent", mapOf("operationId" to id, "phase" to "complete", "percent" to 100,
      "result" to mapOf("ok" to ok, "error" to (error ?: ""), "operationId" to id)))
  }

  private fun withStore(promise: Promise, action: (ILinuxAppStore) -> Unit) {
    val context = requireNotNull(appContext.reactContext)
    val intent = android.content.Intent().setClassName("org.matonos.linux", "org.matonos.linux.LinuxAppStoreService")
    val connection = object : android.content.ServiceConnection {
      override fun onServiceConnected(name: android.content.ComponentName, binder: android.os.IBinder) {
        try { action(ILinuxAppStore.Stub.asInterface(binder)) }
        catch (error: Exception) { promise.reject("STORE_SERVICE", error.message ?: "Store service failed", error) }
      }
      override fun onServiceDisconnected(name: android.content.ComponentName) { }
    }
    storeConnections.add(connection)
    if (!context.bindService(intent, connection, android.content.Context.BIND_AUTO_CREATE))
      promise.reject("STORE_UNAVAILABLE", "MatonOS Linux store service is unavailable", null)
  }

  private fun startStubActivity(action: String, ref: String, operationId: String?): Boolean {
    val context = requireNotNull(appContext.reactContext)
    val deadline = android.os.SystemClock.elapsedRealtime() + 60_000
    fun find() = context.packageManager.queryIntentActivities(
      android.content.Intent(action), android.content.pm.PackageManager.GET_META_DATA)
      .firstOrNull { it.activityInfo.metaData?.getString(STUB_REF_META) == ref }
    var match = find()
    while (match == null && android.os.SystemClock.elapsedRealtime() < deadline) {
      Thread.sleep(500)
      match = find()
    }
    if (match == null) return false
    val intent = android.content.Intent(action)
      .setClassName(match.activityInfo.packageName, match.activityInfo.name)
      .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
    if (!operationId.isNullOrEmpty()) intent.putExtra(STUB_OPERATION_EXTRA, operationId)
    context.startActivity(intent)
    return true
  }

  private fun launchStub(appId: String): Boolean {
    val context = requireNotNull(appContext.reactContext)
    val intent = android.content.Intent(android.content.Intent.ACTION_MAIN).addCategory(android.content.Intent.CATEGORY_LAUNCHER)
    val match = context.packageManager.queryIntentActivities(intent, android.content.pm.PackageManager.GET_META_DATA)
      .firstOrNull { it.activityInfo.metaData?.getString(STUB_REF_META)?.startsWith("app/$appId/") == true }
      ?: return false
    context.startActivity(intent.setClassName(match.activityInfo.packageName, match.activityInfo.name)
      .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
    return true
  }

  private companion object {
    const val FLATHUB_URL = "https://dl.flathub.org/repo/flathub.flatpakrepo"
    const val STUB_INSTALL_ACTION = "org.matonos.linuxhost.INSTALL"
    const val STUB_UPDATE_ACTION = "org.matonos.linuxhost.UPDATE"
    const val STUB_OPERATION_EXTRA = "org.matonos.linuxhost.OPERATION_ID"
    const val STUB_REF_META = "org.matonos.linuxhost.FLATPAK_REF"
  }

  private fun requireClient() = requireNotNull(client) { "MatonOS client is not initialized" }
}
