package org.matonos.flathub.fdroid

import android.content.Context
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Build
import java.security.MessageDigest

object FdroidUpdateChecker {
  const val OFFICIAL_REPOSITORY = "https://f-droid.org/repo"

  fun updates(context: Context): List<Map<String, Any>> {
    val installed = installedVersions(context)
    val client = FdroidIndexClient()
    return FdroidRepositories.list(context).flatMap { repository ->
      val url = repository["url"].toString()
      val fingerprint = repository["fingerprint"].toString()
      try {
        client.updatesCatalogue(context.filesDir, url, fingerprint, installed).onEach { it["repository"] = url }.also { FdroidRepositories.setBroken(context, url, false) }
      } catch (error: Exception) {
        FdroidRepositories.setBroken(context, url, true)
        emptyList()
      }
    }.distinctBy { it["id"] }
  }

  fun checkInBackground(context: Context) {
    val updates = updates(context)
    context.getSharedPreferences("fdroid-updates", Context.MODE_PRIVATE).edit()
      .putStringSet("packages", updates.mapNotNull { it["id"]?.toString() }.toSet())
      .putLong("checkedAt", System.currentTimeMillis())
      .apply()
  }

  @Suppress("DEPRECATION")
  private fun installedVersions(context: Context): List<Map<String, Any>> {
    val pm = context.packageManager
    val flags = if (Build.VERSION.SDK_INT >= 28) PackageManager.GET_SIGNING_CERTIFICATES else PackageManager.GET_SIGNATURES
    return pm.getInstalledPackages(flags).mapNotNull { info ->
      val signers = signerDigests(info)
      if (signers.isEmpty()) return@mapNotNull null
      mapOf(
        "packageName" to info.packageName,
        "versionCode" to versionCode(info),
        "signerSha256" to signers,
      )
    }
  }

  @Suppress("DEPRECATION")
  private fun versionCode(info: PackageInfo): Long =
    if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()

  @Suppress("DEPRECATION")
  private fun signerDigests(info: PackageInfo): List<String> {
    val signatures = if (Build.VERSION.SDK_INT >= 28) {
      info.signingInfo?.apkContentsSigners
    } else {
      info.signatures
    } ?: return emptyList()
    return signatures.map { signature ->
      MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())
        .joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }.sorted()
  }
}
