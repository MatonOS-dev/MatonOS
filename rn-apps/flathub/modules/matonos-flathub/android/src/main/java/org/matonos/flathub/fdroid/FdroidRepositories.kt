package org.matonos.flathub.fdroid

import android.content.Context

object FdroidRepositories {
  const val OFFICIAL_URL = "https://f-droid.org/repo"
  private const val PREFS = "fdroid-repositories"
  private const val REPOSITORIES = "trusted"
  private const val BROKEN = "broken"
  private const val SEEDED = "presets-seeded"

  // Preadded on first use, removable like any user repo. Fingerprints are the
  // signers of each repo's signed index, checked 2026-11-15.
  private val PRESETS = listOf(
    "https://apt.izzysoft.de/fdroid/repo" to "3bf0d6abfeae2f401707b6d966be743bf0eee49c2561b9ba39073711f628937a",
    "https://microg.org/fdroid/repo" to "9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165",
    "https://guardianproject.info/fdroid/repo" to "b7c2eefd8dac7806af67dfcd92eb18126bc08312a7f2d6f3862e46013c7a6135",
    "https://cdn.kde.org/android/stable-releases/fdroid/repo" to "13784ba6c80ff4e2181e55c56f961eed5844cea16870d3b38d58780b85e1158f",
    // Upstream app repos, from Neo-Store's default list (checked 2026-11-15).
    "https://archive.newpipe.net/fdroid/repo" to "e2402c78f9b97c6c89e97db914a2751fda1d02fe2039cc0897a462bdb57e7501",
    "https://www.collaboraoffice.com/downloads/fdroid/repo" to "573258c84e149b5f4d9299e7434b2b69a8410372921d4ae586ba91ec767892cc",
    "https://mobileapp.bitwarden.com/fdroid/repo" to "bc54ea6fd1cd5175bcccc47c561c5726e1c3ed7e686b6db4b18bac843a3efe6c",
    "https://molly.im/fdroid/foss/fdroid/repo" to "5198daef37fc23c14d5ee32305b2af45787bd7df2034de33ad302bdb3446df74",
    "https://briarproject.org/fdroid/repo" to "1fb874bee7276d28ecb2c9b06e8a122ec4bcb4008161436ce474c257cbf49bd6",
    "https://fdroid.getsession.org/fdroid/repo" to "db0e5297eb65cc22d6bd93c869943bdcfcb6a07dc69a48a0dd8c7ba698ec04e6",
    "https://releases.threema.ch/fdroid/repo" to "5734e753899b25775d90fe85362a49866e05ac4f83c05bef5a92880d2910639e",
    "https://static.cryptomator.org/android/fdroid/repo" to "f7c3ec3b0d588d3cb52983e9eb1a7421c93d4339a286398e71d7b651e8d8ecdd",
    "https://freeyourgadget.codeberg.page/fdroid/repo" to "cd381eccc465ab324e21bcc335895615e07e70ee11e9fd1df3c020c5194f00b2",
    "https://app.futo.org/fdroid/repo" to "39d47869d29cbfce4691d9f7e6946a7b6d7e6ff4883497e6e675744ecdfa6d6d",
    "https://fdroid.ironfoxoss.org/fdroid/repo" to "c5e291b5a571f9c8cd9a9799c2c94e02ec9703948893f2ca756d67b94204f904",
    "https://breezy-weather.github.io/fdroid-repo/fdroid/repo" to "3480a7bb2a296d8f98cb90d2309199b5b9519c1b31978dbcd877adb102af35ee",
    "https://thunderbird.github.io/fdroid-thunderbird/repo" to "8b86e5d48983f0875f7eb7a1b2f91b225ee5b997e463e3d63d0e2556e53666be",
  )

  private fun seedPresets(context: Context) {
    val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    if (prefs.getBoolean(SEEDED, false)) return
    val repos = prefs.getStringSet(REPOSITORIES, emptySet()).orEmpty().toMutableSet()
    for ((url, fingerprint) in PRESETS) {
      if (repos.none { it.substringBefore('\t') == url }) repos.add("$url\t$fingerprint")
    }
    prefs.edit().putStringSet(REPOSITORIES, repos).putBoolean(SEEDED, true).apply()
  }

  fun list(context: Context): List<Map<String, Any>> {
    seedPresets(context)
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
    seedPresets(context) // so a removed preset is never re-added later
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
