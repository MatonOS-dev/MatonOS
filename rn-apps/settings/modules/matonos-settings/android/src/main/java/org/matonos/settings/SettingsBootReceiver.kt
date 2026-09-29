package org.matonos.settings.nativebridge

import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Keeps the drawer alias disabled except on the live image. The property is
 * read through Android's public getprop tool; failure always leaves it hidden. */
class SettingsBootReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent) {
    if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
    val pending = goAsync()
    executor.execute {
      try {
        val process = ProcessBuilder("/system/bin/getprop", "ro.boot.matonos.live")
          .redirectErrorStream(true).start()
        val finished = process.waitFor(1200, TimeUnit.MILLISECONDS)
        val isLive = finished && process.exitValue() == 0 && process.inputStream.bufferedReader().use { it.readText().trim() } == "1"
        if (!finished) process.destroyForcibly()
        val alias = ComponentName(context, "org.matonos.settings.InstallAlias")
        context.packageManager.setComponentEnabledSetting(
          alias,
          if (isLive) PackageManager.COMPONENT_ENABLED_STATE_ENABLED else PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
          PackageManager.DONT_KILL_APP,
        )
      } catch (_: Exception) {
        // Fail closed: the install shortcut never appears if live mode is unknown.
      } finally {
        pending.finish()
      }
    }
  }

  private companion object {
    val executor = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "matonos-settings-boot").apply { isDaemon = true } }
  }
}
