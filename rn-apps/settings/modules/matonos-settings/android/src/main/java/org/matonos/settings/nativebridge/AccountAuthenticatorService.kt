package org.matonos.settings.nativebridge

import android.app.Service
import android.content.Intent
import android.os.IBinder

class AccountAuthenticatorService : Service() {
  private lateinit var authenticator: AccountAuthenticator
  override fun onCreate() { super.onCreate(); authenticator = AccountAuthenticator(this) }
  override fun onBind(intent: Intent?): IBinder = authenticator.iBinder
}
