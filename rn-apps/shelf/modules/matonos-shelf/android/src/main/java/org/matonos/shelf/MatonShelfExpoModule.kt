package org.matonos.shelf

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

/** Expo native API for the independent shelf process and overlay service. */
class MatonShelfExpoModule : Module() {
  private var packageReceiver: BroadcastReceiver? = null
  private var wallpaperReceiver: BroadcastReceiver? = null

  override fun definition() = ModuleDefinition {
    Name("MatonShelf")
    Events("shelfStateChanged", "packagesChanged")

    OnCreate {
      val context = requireNotNull(appContext.reactContext)
      active = this@MatonShelfExpoModule
      val packages = IntentFilter().apply {
        addAction(Intent.ACTION_PACKAGE_ADDED)
        addAction(Intent.ACTION_PACKAGE_REMOVED)
        addAction(Intent.ACTION_PACKAGE_CHANGED)
        addDataScheme("package")
      }
      packageReceiver = object : BroadcastReceiver() {
        override fun onReceive(ignored: Context?, intent: Intent?) {
          sendEvent("packagesChanged", mapOf(
            "type" to (intent?.action ?: "changed"),
            "packageName" to (intent?.data?.schemeSpecificPart ?: ""),
          ))
        }
      }
      wallpaperReceiver = object : BroadcastReceiver() {
        override fun onReceive(ignored: Context?, intent: Intent?) {
          sendEvent("shelfStateChanged", mapOf("type" to "wallpaperChanged"))
        }
      }
      if (android.os.Build.VERSION.SDK_INT >= 33) {
        context.registerReceiver(packageReceiver, packages, Context.RECEIVER_NOT_EXPORTED)
        context.registerReceiver(wallpaperReceiver, IntentFilter(Intent.ACTION_WALLPAPER_CHANGED), Context.RECEIVER_NOT_EXPORTED)
      } else {
        context.registerReceiver(packageReceiver, packages)
        context.registerReceiver(wallpaperReceiver, IntentFilter(Intent.ACTION_WALLPAPER_CHANGED))
      }
    }

    OnDestroy {
      val context = appContext.reactContext
      try { if (context != null) packageReceiver?.let(context::unregisterReceiver) } catch (_: Exception) { }
      try { if (context != null) wallpaperReceiver?.let(context::unregisterReceiver) } catch (_: Exception) { }
      packageReceiver = null
      wallpaperReceiver = null
      if (active === this@MatonShelfExpoModule) active = null
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
      ShelfService.dispatchLaunch(context, Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setComponent(component))
      true
    }
    AsyncFunction("getWallpaperSeedColor") {
      try {
        WallpaperManager.getInstance(requireNotNull(appContext.reactContext))
          .getWallpaperColors(WallpaperManager.FLAG_SYSTEM)?.primaryColor?.toArgb() ?: 0xff6750a4.toInt()
      } catch (_: Exception) { 0xff6750a4.toInt() }
    }
    AsyncFunction("getShelfState") {
      val context = requireNotNull(appContext.reactContext)
      mapOf("expanded" to ShelfService.isShelfExpanded(), "homeVisible" to ShelfService.isHomeVisible(), "panel" to ShelfService.getActivePanel(), "threeButtonMode" to ShelfService.isThreeButtonMode(context))
    }
    AsyncFunction("getPinnedApps") { ShelfService.getPinnedApps(requireNotNull(appContext.reactContext)).toList() }
    AsyncFunction("togglePinnedApp") { packageName: String -> ShelfService.togglePinnedApp(requireNotNull(appContext.reactContext), packageName) }
    Function("setShelfExpanded") { expanded: Boolean -> ShelfService.setShelfExpanded(expanded) }
    Function("setThreeButtonMode") { enabled: Boolean -> ShelfService.setThreeButtonMode(requireNotNull(appContext.reactContext), enabled) }
    Function("openPanel") { panel: String -> ShelfService.openPanel(requireNotNull(appContext.reactContext), panel) }
    Function("goHome") { ShelfService.goHome(requireNotNull(appContext.reactContext)) }
    Function("reportSurfaceFailure") { error: String -> ShelfService.fallbackToJavaShelf(error) }
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
      Log.w("MatonOSShelf", "Could not cache app icon", error)
      ""
    }
  }

  companion object {
    @Volatile private var active: MatonShelfExpoModule? = null
    @JvmStatic fun notifyShelfState() {
      active?.sendEvent("shelfStateChanged", mapOf(
        "type" to "stateChanged",
        "expanded" to ShelfService.isShelfExpanded(),
        "homeVisible" to ShelfService.isHomeVisible(),
        "panel" to ShelfService.getActivePanel(),
      ))
    }
  }
}
