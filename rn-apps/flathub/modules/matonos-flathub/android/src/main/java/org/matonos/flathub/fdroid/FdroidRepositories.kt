package org.matonos.flathub.fdroid

import android.content.Context

object FdroidRepositories {
  const val OFFICIAL_URL = "https://f-droid.org/repo"
  private const val PREFS = "fdroid-repositories"
  private const val REPOSITORIES = "trusted"
  private const val BROKEN = "broken"

  fun list(context: Context): List<Map<String, Any>> {
    val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    val values = mutableListOf(mapOf("url" to OFFICIAL_URL, "fingerprint" to FdroidIndexClient.OFFICIAL_FINGERPRINT, "official" to true, "broken" to false))
    for (item in prefs.getStringSet(REPOSITORIES, emptySet()).orEmpty()) {
      val parts = item.split("\t", limit = 2)
      if (parts.size == 2) values.add(mapOf("url" to parts[0], "fingerprint" to parts[1], "official" to false, "broken" to prefs.getStringSet(BROKEN, emptySet()).orEmpty().contains(parts[0])))
    }
    return values
  }

  fun add(context: Context, url: String, fingerprint: String) {
    val normalizedUrl = FdroidRepository.normalizeUrl(url)
    val normalizedFingerprint = FdroidRepository.normalizeFingerprint(fingerprint)
    require(normalizedUrl != OFFICIAL_URL) { "The built-in F-Droid repository is already enabled" }
    val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    val repos = prefs.getStringSet(REPOSITORIES, emptySet()).orEmpty().toMutableSet()
    repos.removeAll { it.substringBefore('\t') == normalizedUrl }
    repos.add("$normalizedUrl\t$normalizedFingerprint")
    val broken = prefs.getStringSet(BROKEN, emptySet()).orEmpty().toMutableSet()
    broken.add(normalizedUrl) // A new source is unverified until its signed index has been checked.
    prefs.edit().putStringSet(REPOSITORIES, repos).putStringSet(BROKEN, broken).apply()
  }

  fun remove(context: Context, url: String) {
    val normalizedUrl = FdroidRepository.normalizeUrl(url)
    require(normalizedUrl != OFFICIAL_URL) { "The built-in F-Droid repository cannot be removed" }
    val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    prefs.edit()
      .putStringSet(REPOSITORIES, prefs.getStringSet(REPOSITORIES, emptySet()).orEmpty().filterNot { it.substringBefore('\t') == normalizedUrl }.toSet())
      .putStringSet(BROKEN, prefs.getStringSet(BROKEN, emptySet()).orEmpty().filterNot { it == normalizedUrl }.toSet())
      .apply()
  }

  fun setBroken(context: Context, url: String, broken: Boolean) {
    if (url == OFFICIAL_URL) return
    val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    val brokenRepos = prefs.getStringSet(BROKEN, emptySet()).orEmpty().toMutableSet()
    if (broken) brokenRepos.add(url) else brokenRepos.remove(url)
    prefs.edit().putStringSet(BROKEN, brokenRepos).apply()
  }
}
