package org.matonos.settings.nativebridge

import android.accounts.Account
import android.accounts.AccountManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/** Receives the first-boot account handoff from the platform-signed system bridge. */
class SetupAccountReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent) {
    try {
      val username = intent.getStringExtra("username")?.takeIf { it.isNotBlank() }
        ?: throw IllegalArgumentException("missing username")
      val refreshToken = intent.getStringExtra("refresh_token")?.takeIf { it.isNotBlank() }
        ?: throw IllegalArgumentException("missing refresh token")
      val machineId = intent.getStringExtra("machine_id")?.takeIf { it.isNotBlank() }
        ?: throw IllegalArgumentException("missing machine id")
      val manager = AccountManager.get(context)
      val account = Account(username, AccountAuthenticator.TYPE)
      val existing = manager.getAccountsByType(AccountAuthenticator.TYPE)
      if (existing.none { it.name.equals(username, ignoreCase = true) }) {
        if (existing.isNotEmpty()) throw IllegalStateException("a different MatonOS account already exists")
        if (!manager.addAccountExplicitly(account, refreshToken, null))
          throw IllegalStateException("AccountManager.addAccountExplicitly returned false")
      } else {
        manager.setPassword(account, refreshToken)
      }
      manager.setUserData(account, "machine_id", machineId)
    } catch (error: Exception) {
      Log.e("MatonSetupAccount", "Failed to store first-boot MatonOS account in AccountManager", error)
    }
  }
}
