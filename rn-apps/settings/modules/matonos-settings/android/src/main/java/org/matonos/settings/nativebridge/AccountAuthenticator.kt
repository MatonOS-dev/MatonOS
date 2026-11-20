package org.matonos.settings.nativebridge

import android.accounts.AbstractAccountAuthenticator
import android.accounts.Account
import android.accounts.AccountAuthenticatorResponse
import android.accounts.AccountManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Binder
import android.content.pm.PackageManager

class AccountAuthenticator(private val context: Context) : AbstractAccountAuthenticator(context) {
  companion object {
    const val TYPE = "org.matonos.account"
    const val TOKEN_TYPE = "session"
    const val EXTRA_FLOW = "matonos.account.flow"
    const val EXTRA_CALLER = "matonos.account.caller"
    const val EXTRA_MACHINE_ID = "org.matonos.account.machine_id"
    private const val EXTRA_ACCOUNT_NAME = "matonos.account.name"
  }

  private fun flow(response: AccountAuthenticatorResponse?, kind: String, account: Account? = null, caller: String? = null): Bundle {
    val intent = Intent(context, org.matonos.settings.AccountFlowActivity::class.java)
      .putExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE, response)
      .putExtra(EXTRA_FLOW, kind)
    account?.let { intent.putExtra(EXTRA_ACCOUNT_NAME, it.name) }
    caller?.let { intent.putExtra(EXTRA_CALLER, it) }
    return Bundle().apply { putParcelable(AccountManager.KEY_INTENT, intent) }
  }

  override fun addAccount(response: AccountAuthenticatorResponse, accountType: String, authTokenType: String?, requiredFeatures: Array<out String>?, options: Bundle?): Bundle {
    val existing = AccountManager.get(context).getAccountsByType(TYPE)
    if (existing.isNotEmpty()) return Bundle().apply { putString(AccountManager.KEY_ERROR_MESSAGE, "Only one MatonOS account can be added") }
    return flow(response, "signin")
  }

  override fun getAuthToken(response: AccountAuthenticatorResponse, account: Account, authTokenType: String, options: Bundle?): Bundle {
    if (authTokenType != TOKEN_TYPE) return Bundle().apply { putString(AccountManager.KEY_ERROR_MESSAGE, "Unsupported MatonOS auth token type") }
    val requestedPackage = options?.getString(AccountManager.KEY_ANDROID_PACKAGE_NAME)
    val requestedUid = requestedPackage?.let { try { context.packageManager.getPackageUid(it, 0) } catch (_: PackageManager.NameNotFoundException) { -1 } } ?: -1
    val platformCaller = requestedUid >= 0 && context.packageManager.checkSignatures("android", requestedPackage!!) == PackageManager.SIGNATURE_MATCH
    val manager = AccountManager.get(context)
    val token = manager.peekAuthToken(account, TOKEN_TYPE)
    if (platformCaller && !token.isNullOrBlank()) return tokenBundle(account, token)
    return flow(response, if (platformCaller) "signin" else "approve", account, requestedPackage)
  }

  private fun tokenBundle(account: Account, token: String) = Bundle().apply {
    putString(AccountManager.KEY_ACCOUNT_NAME, account.name)
    putString(AccountManager.KEY_ACCOUNT_TYPE, TYPE)
    putString(AccountManager.KEY_AUTHTOKEN, token)
    putString(EXTRA_MACHINE_ID, AccountManager.get(context).getUserData(account, "machine_id"))
  }

  override fun getAuthTokenLabel(authTokenType: String?): String = "MatonOS session"
  override fun confirmCredentials(response: AccountAuthenticatorResponse?, account: Account?, options: Bundle?): Bundle = flow(response, "signin", account)
  override fun editProperties(response: AccountAuthenticatorResponse?, accountType: String?): Bundle = flow(response, "signin")
  override fun updateCredentials(response: AccountAuthenticatorResponse?, account: Account?, authTokenType: String?, options: Bundle?): Bundle = flow(response, "signin", account)
  override fun hasFeatures(response: AccountAuthenticatorResponse?, account: Account?, features: Array<out String>?): Bundle = Bundle().apply { putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true) }
}
