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
import android.util.Base64
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

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
    if (platformCaller && !token.isNullOrBlank() && tokenExpiry(token) > System.currentTimeMillis() / 1000 + 300) return tokenBundle(account, token)
    if (platformCaller) {
      val refresh = manager.getPassword(account)
      val machineId = manager.getUserData(account, "machine_id")
      if (!refresh.isNullOrBlank() && !machineId.isNullOrBlank()) {
        try {
          val renewed = refreshCredentials(refresh, machineId)
          manager.setAuthToken(account, TOKEN_TYPE, renewed.first)
          manager.setPassword(account, renewed.second)
          return tokenBundle(account, renewed.first)
        } catch (e: RefreshFailure) {
          if (e.unauthorized) {
            if (!token.isNullOrBlank()) manager.invalidateAuthToken(TYPE, token)
            manager.setPassword(account, null)
            return flow(response, "signin", account)
          }
          return Bundle().apply { putString(AccountManager.KEY_ERROR_MESSAGE, e.message ?: "MatonOS session refresh failed") }
        }
      }
    }
    return flow(response, if (platformCaller) "signin" else "approve", account, requestedPackage)
  }

  internal data class RefreshFailure(val unauthorized: Boolean, override val message: String) : Exception(message)

  internal fun tokenExpiry(token: String): Long = try {
    val payload = token.split('.')[1]
    JSONObject(String(Base64.decode(payload, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING), Charsets.UTF_8)).optLong("exp", 0)
  } catch (_: Exception) { 0 }

  internal fun refreshCredentials(refreshToken: String, machineId: String): Pair<String, String> {
    val connection = (URL("https://maton.hanro50.net.za/api/account/refresh").openConnection() as HttpURLConnection).apply {
      requestMethod = "POST"
      connectTimeout = 10_000
      readTimeout = 10_000
      doOutput = true
      setRequestProperty("Content-Type", "application/json")
      setRequestProperty("X-Maton-Machine-Id", machineId)
    }
    try {
      val body = JSONObject().put("refresh_token", refreshToken).toString().toByteArray(Charsets.UTF_8)
      connection.outputStream.use { it.write(body) }
      val status = connection.responseCode
      if (status == 401) throw RefreshFailure(true, "MatonOS session expired. Sign in again.")
      if (status !in 200..299) throw RefreshFailure(false, "MatonOS session refresh failed ($status)")
      val value = JSONObject(connection.inputStream.bufferedReader().use { it.readText() })
      val token = value.optString("token")
      val nextRefresh = value.optString("refresh_token")
      if (token.isBlank() || nextRefresh.isBlank()) throw RefreshFailure(false, "MatonOS server returned an incomplete refresh")
      return token to nextRefresh
    } catch (e: RefreshFailure) {
      throw e
    } catch (e: Exception) {
      throw RefreshFailure(false, e.message ?: "MatonOS session refresh failed")
    } finally { connection.disconnect() }
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
