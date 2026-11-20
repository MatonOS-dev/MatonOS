package org.matonos.settings

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.accounts.AccountAuthenticatorResponse
import android.accounts.AccountManager
import org.matonos.settings.nativebridge.AccountAuthenticator

/** Hands AccountManager sign-in/add flows to the Settings app's RN account screen. */
class AccountFlowActivity : Activity() {
  companion object {
    @Volatile private var active: AccountFlowActivity? = null
    fun hasPendingFlow(): Boolean = active != null
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    active = this
    val flow = intent.getStringExtra(AccountAuthenticator.EXTRA_FLOW) ?: "signin"
    val caller = intent.getStringExtra(AccountAuthenticator.EXTRA_CALLER)
    val callerQuery = caller?.let { "&caller=${android.net.Uri.encode(it)}" } ?: ""
    // MainActivity lives in the generated app module, which this library module cannot see.
    val launch = Intent(Intent.ACTION_VIEW).setClassName(this, "$packageName.MainActivity")
      .setData(android.net.Uri.parse("matonos-settings://account?flow=$flow$callerQuery"))
      .putExtra(AccountAuthenticator.EXTRA_FLOW, flow)
      .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
    startActivityForResult(launch, 41)
  }

  @Deprecated("Forward the RN Settings result to AccountManager")
  override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
    super.onActivityResult(requestCode, resultCode, data)
    if (requestCode == 41) {
      @Suppress("DEPRECATION")
      val response = intent.getParcelableExtra<AccountAuthenticatorResponse>(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE)
      if (resultCode == RESULT_OK && data?.extras != null) response?.onResult(data.extras!!)
      else response?.onError(4, "MatonOS account sign-in was cancelled")
      finish()
    }
  }

  override fun onDestroy() { if (active === this) active = null; super.onDestroy() }
}
