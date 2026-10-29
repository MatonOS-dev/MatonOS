package org.matonos.flathub.nativebridge

import expo.modules.kotlin.Promise
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import org.matonos.client.BridgeMode
import org.matonos.client.MatonOS
import org.matonos.client.MatonosClient
import org.matonos.linux.ILinuxAppStore
import org.matonos.linux.ILinuxAppStoreCallback
import org.matonos.flathub.fdroid.FdroidIndexClient
import org.matonos.flathub.fdroid.FdroidApkInstaller
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ConcurrentHashMap

class MatonOSExpoModule : Module() {
  private var client: MatonosClient? = null
  private val listeners = ConcurrentHashMap<String, MatonosClient.EventListener>()
  private val storeConnections = ConcurrentHashMap.newKeySet<android.content.ServiceConnection>()
  private val fdroidIndex = FdroidIndexClient()

  override fun definition() = ModuleDefinition {
    Name("MatonOSClient")
    Events("matonosAreaEvent", "linuxAppStoreEvent")
    OnCreate {
      client = MatonOS.init(requireNotNull(appContext.reactContext), BridgeMode.OPTIONAL)
      fdroidEventSink = { operationId, phase, percent, bytesDone, bytesTotal, result ->
        val event = mutableMapOf<String, Any>("source" to "F-Droid", "operationId" to operationId, "phase" to phase, "percent" to percent)
        if (bytesDone >= 0) event["bytesDone"] = bytesDone
        if (bytesTotal >= 0) event["bytesTotal"] = bytesTotal
        if (result != null) event["result"] = result
        sendEvent("linuxAppStoreEvent", event)
      }
    }
    OnDestroy {
      val context = appContext.reactContext
      if (context != null) storeConnections.forEach { try { context.unbindService(it) } catch (_: Exception) { } }
      storeConnections.clear()
      fdroidEventSink = null
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
    AsyncFunction("fdroidSearch") { repository: String, query: String ->
      fdroidIndex.search(requireNotNull(appContext.reactContext).filesDir, repository, query)
    }
    AsyncFunction("fdroidDetails") { repository: String, packageName: String ->
      fdroidIndex.details(requireNotNull(appContext.reactContext).filesDir, repository, packageName)
        ?: throw IllegalArgumentException("F-Droid app not found: $packageName")
    }
    AsyncFunction("fdroidInstalled") { repository: String ->
      val context = requireNotNull(appContext.reactContext)
      @Suppress("DEPRECATION")
      val packageNames = context.packageManager.getInstalledPackages(0).map { it.packageName }
      fdroidIndex.installedCatalogue(context.filesDir, repository, packageNames)
    }
    AsyncFunction("fdroidInstall") { repository: String, packageName: String, operationId: String, promise: Promise ->
      val context = requireNotNull(appContext.reactContext)
      Thread {
        val cache = File(context.cacheDir, "fdroid-install/$operationId")
        try {
          val app = fdroidIndex.details(context.filesDir, repository, packageName)
            ?: throw IllegalArgumentException("F-Droid app not found: $packageName")
          @Suppress("UNCHECKED_CAST")
          val apkFiles = app["apkFiles"] as? List<Map<String, String>> ?: emptyList()
          if (apkFiles.isEmpty()) throw IllegalArgumentException("F-Droid app has no downloadable APK files")
          if (!cache.mkdirs() && !cache.isDirectory) throw IllegalStateException("Cannot create APK download cache")
          val downloads = apkFiles.mapIndexed { index, item ->
            val name = item["name"] ?: throw IllegalArgumentException("F-Droid APK has no name")
            val hash = item["sha256"] ?: throw IllegalArgumentException("F-Droid APK has no SHA-256")
            val file = File(cache, "$index.apk")
            downloadApk(repository, name, file, operationId, index, apkFiles.size)
            file to hash
          }
          val installer = context.packageManager.packageInstaller
          FdroidApkInstaller().verifyThenCommit(downloads.map { it.first }, downloads.map { it.second }) { verified ->
            val params = android.content.pm.PackageInstaller.SessionParams(android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL)
            params.setAppPackageName(packageName)
            if (android.os.Build.VERSION.SDK_INT >= 34) params.setRequestUpdateOwnership(true)
            val sessionId = installer.createSession(params)
            try {
              installer.openSession(sessionId).use { session ->
                verified.forEachIndexed { index, file ->
                  val archive = context.packageManager.getPackageArchiveInfo(file.absolutePath, 0)
                    ?: throw IllegalArgumentException("F-Droid download is not a readable APK")
                  require(archive.packageName == packageName) { "F-Droid APK package name does not match the catalogue" }
                  val splitName = archive.splitNames?.firstOrNull()
                  val apkName = if (splitName.isNullOrEmpty()) "base.apk" else "split_$splitName.apk"
                  FileInputStream(file).use { input -> session.openWrite(apkName, 0, file.length()).use { output ->
                    input.copyTo(output); session.fsync(output)
                  } }
                }
                session.commit(packageResultIntent(context, operationId, "install").intentSender)
              }
            } catch (error: Exception) {
              try { installer.abandonSession(sessionId) } catch (_: Exception) { }
              throw error
            }
          }
          promise.resolve(true)
        } catch (error: Exception) {
          emitFdroidResult(operationId, false, error.message ?: "F-Droid installation failed")
          promise.reject("FDROID_INSTALL", error.message ?: "F-Droid installation failed", error)
        } finally {
          cache.deleteRecursively()
        }
      }.start()
    }
    AsyncFunction("fdroidUninstall") { packageName: String, operationId: String, promise: Promise ->
      try {
        requireNotNull(appContext.reactContext).packageManager.packageInstaller.uninstall(
          packageName, packageResultIntent(requireNotNull(appContext.reactContext), operationId, "uninstall").intentSender)
        promise.resolve(true)
      } catch (error: Exception) {
        emitFdroidResult(operationId, false, error.message ?: "F-Droid uninstall failed")
        promise.reject("FDROID_UNINSTALL", error.message ?: "F-Droid uninstall failed", error)
      }
    }
  }

  private fun downloadApk(repository: String, name: String, destination: File, operationId: String, fileIndex: Int, fileCount: Int) {
    val safeName = name.trimStart('/')
    require(safeName.split('/').none { it == ".." }) { "Invalid F-Droid APK path" }
    val connection = URL(repository.trimEnd('/') + "/" + safeName).openConnection() as HttpURLConnection
    connection.connectTimeout = 20_000; connection.readTimeout = 60_000; connection.instanceFollowRedirects = true
    connection.setRequestProperty("User-Agent", "MatonOS-Software-Center/1.0")
    try {
      val code = connection.responseCode
      if (code !in 200..299) throw IllegalStateException("F-Droid returned HTTP $code for APK")
      val total = connection.contentLengthLong
      var done = 0L
      connection.inputStream.use { input -> FileOutputStream(destination).use { output ->
        val buffer = ByteArray(16 * 1024)
        while (true) {
          val count = input.read(buffer)
          if (count < 0) break
          output.write(buffer, 0, count); done += count
          val percent = if (total > 0) ((fileIndex * 100L + done * 100L / total) / fileCount).toInt() else -1
          emitFdroidProgress(operationId, "Downloading APK ${fileIndex + 1} of $fileCount", percent, done, total)
        }
        output.fd.sync()
      } }
    } finally { connection.disconnect() }
  }

  private fun packageResultIntent(context: android.content.Context, operationId: String, kind: String): android.app.PendingIntent {
    val intent = android.content.Intent(context, FdroidInstallReceiver::class.java)
      .putExtra(FdroidInstallReceiver.EXTRA_OPERATION_ID, operationId)
      .putExtra(FdroidInstallReceiver.EXTRA_KIND, kind)
    return android.app.PendingIntent.getBroadcast(context, (operationId + kind).hashCode(), intent,
      android.app.PendingIntent.FLAG_UPDATE_CURRENT or android.app.PendingIntent.FLAG_MUTABLE)
  }

  private fun emitFdroidProgress(id: String, phase: String, percent: Int, done: Long, total: Long) {
    fdroidEventSink?.invoke(id, phase, percent, done, total, null)
  }

  private fun emitFdroidResult(id: String, ok: Boolean, error: String?) {
    fdroidEventSink?.invoke(id, "complete", 100, -1, -1,
      mapOf("ok" to ok, "error" to (error ?: ""), "operationId" to id))
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

  companion object {
    @Volatile var fdroidEventSink: ((String, String, Int, Long, Long, Map<String, Any>?) -> Unit)? = null
    const val FLATHUB_URL = "https://dl.flathub.org/repo/flathub.flatpakrepo"
    const val STUB_INSTALL_ACTION = "org.matonos.linuxhost.INSTALL"
    const val STUB_UPDATE_ACTION = "org.matonos.linuxhost.UPDATE"
    const val STUB_OPERATION_EXTRA = "org.matonos.linuxhost.OPERATION_ID"
    const val STUB_REF_META = "org.matonos.linuxhost.FLATPAK_REF"
  }

  private fun requireClient() = requireNotNull(client) { "MatonOS client is not initialized" }
}
