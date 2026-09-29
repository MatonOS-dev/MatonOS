package org.matonos.settings

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle

/** Turns the launcher alias tap into the Expo Router install deep link. */
class InstallAliasActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    startActivity(
      Intent(Intent.ACTION_VIEW).setClassName(this, "org.matonos.settings.MainActivity")
        .setData(Uri.parse("matonos-settings://install"))
        .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
    )
    finish()
  }
}
