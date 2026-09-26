package org.matonos.shell

import android.app.WallpaperManager
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ResolveInfo
import android.graphics.Bitmap
import android.graphics.Canvas
import android.net.Uri
import android.util.Log
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import java.io.File
import java.io.FileOutputStream
import java.util.Locale

/** Native operations needed by the full-screen Home, Drawer and Recents app. */
class MatonShellExpoModule : Module() {
  private var packageReceiver: BroadcastReceiver? = null
  private var wallpaperReceiver: BroadcastReceiver? = null

  override fun definition() = ModuleDefinition {
    Name("MatonShell")
    Events("packagesChanged", "wallpaperChanged")

    OnCreate {
      val context = requireNotNull(appContext.reactContext)
      val packages = IntentFilter().apply {
        addAction(Intent.ACTION_PACKAGE_ADDED)
        addAction(Intent.ACTION_PACKAGE_REMOVED)
        addAction(Intent.ACTION_PACKAGE_CHANGED)
        addDataScheme("package")
      }
      packageReceiver = object : BroadcastReceiver() {
        override fun onReceive(ignored: Context?, intent: Intent?) {
          sendEvent("packagesChanged", mapOf("type" to (intent?.action ?: "changed"), "packageName" to (intent?.data?.schemeSpecificPart ?: "")))
        }
      }
      wallpaperReceiver = object : BroadcastReceiver() {
        override fun onReceive(ignored: Context?, intent: Intent?) { sendEvent("wallpaperChanged", emptyMap<String, String>()) }
      }
      if (android.os.Build.VERSION.SDK_INT >= 33)
        context.registerReceiver(packageReceiver, packages, Context.RECEIVER_NOT_EXPORTED)
      else context.registerReceiver(packageReceiver, packages)
      if (android.os.Build.VERSION.SDK_INT >= 33)
        context.registerReceiver(wallpaperReceiver, IntentFilter(Intent.ACTION_WALLPAPER_CHANGED), Context.RECEIVER_NOT_EXPORTED)
      else context.registerReceiver(wallpaperReceiver, IntentFilter(Intent.ACTION_WALLPAPER_CHANGED))
    }

    OnDestroy {
      try { appContext.reactContext?.let { context -> packageReceiver?.let(context::unregisterReceiver) } }
      catch (_: Exception) { }
      try { appContext.reactContext?.let { context -> wallpaperReceiver?.let(context::unregisterReceiver) } }
      catch (_: Exception) { }
      packageReceiver = null
      wallpaperReceiver = null
    }

    AsyncFunction("getLauncherApps") {
      val context = requireNotNull(appContext.reactContext)
      launcherApps(context).map { info ->
        val pkg = info.activityInfo.packageName
        mapOf(
          "packageName" to pkg,
          "component" to ComponentName(pkg, info.activityInfo.name).flattenToString(),
          "label" to info.loadLabel(context.packageManager).toString(),
          "iconUri" to cacheIcon(context, info),
        )
      }
    }
    AsyncFunction("launchApp") { componentName: String ->
      val context = requireNotNull(appContext.reactContext)
      val component = ComponentName.unflattenFromString(componentName) ?: return@AsyncFunction false
      val launch = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        .setComponent(component).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
      try { context.startActivity(launch); true }
      catch (error: RuntimeException) { Log.e("MatonOSShell", "Could not launch $componentName", error); false }
    }
    AsyncFunction("getWallpaperSeedColor") {
      try {
        WallpaperManager.getInstance(requireNotNull(appContext.reactContext))
          .getWallpaperColors(WallpaperManager.FLAG_SYSTEM)?.primaryColor?.toArgb() ?: 0xff6750a4.toInt()
      } catch (_: Exception) { 0xff6750a4.toInt() }
    }
    AsyncFunction("togglePinnedApp") { packageName: String ->
      requireNotNull(appContext.reactContext).sendBroadcast(
        Intent("org.matonos.shelf.TOGGLE_PIN")
          .setComponent(ComponentName("org.matonos.shelf", "org.matonos.shelf.BootReceiver"))
          .putExtra("packageName", packageName),
      )
      true
    }
    Function("goHome") {
      val context = requireNotNull(appContext.reactContext)
      context.startActivity(Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME)
        .addCategory(Intent.CATEGORY_DEFAULT).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
    Function("reportSurfaceFailure") { surfaceName: String, error: String ->
      requireNotNull(appContext.reactContext).sendBroadcast(
        Intent(ACTION_SURFACE_FAILED).setPackage(requireNotNull(appContext.reactContext).packageName)
          .putExtra("surface", surfaceName).putExtra("error", error),
      )
    }
  }

  private fun launcherApps(context: Context): List<ResolveInfo> {
    val query = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
    return context.packageManager.queryIntentActivities(query, 0)
      .filter { it.activityInfo.packageName != context.packageName && it.activityInfo.packageName != "com.android.systemui" }
      .sortedBy { it.loadLabel(context.packageManager).toString().lowercase(Locale.ROOT) }
  }

  private fun cacheIcon(context: Context, info: ResolveInfo): String {
    val directory = File(context.cacheDir, "launcher-icons")
    val file = File(directory, info.activityInfo.packageName.replace(Regex("[^A-Za-z0-9._-]"), "_") + ".png")
    if (file.isFile) return Uri.fromFile(file).toString()
    return try {
      directory.mkdirs()
      val size = (96 * context.resources.displayMetrics.density).toInt().coerceAtLeast(64)
      val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
      val canvas = Canvas(bitmap)
      info.loadIcon(context.packageManager).also { it.setBounds(0, 0, size, size) }.draw(canvas)
      FileOutputStream(file).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
      bitmap.recycle()
      Uri.fromFile(file).toString()
    } catch (error: Exception) {
      Log.w("MatonOSShell", "Could not cache app icon", error)
      ""
    }
  }

  companion object {
    private const val ACTION_SURFACE_FAILED = "org.matonos.shell.RN_SURFACE_FAILED"
    @JvmStatic fun surfaceFailureAction(): String = ACTION_SURFACE_FAILED
  }
}
