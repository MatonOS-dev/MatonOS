package org.matonos.flathub.nativebridge

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller

class FdroidInstallReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent) {
    val operationId = intent.getStringExtra(EXTRA_OPERATION_ID) ?: return
    val kind = intent.getStringExtra(EXTRA_KIND) ?: "install"
    val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
    if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
      val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
      if (confirm != null) {
        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        context.startActivity(confirm)
      }
      MatonOSExpoModule.fdroidEventSink?.invoke(operationId, "Waiting for package confirmation", -1, -1, -1, null)
      return
    }
    val ok = status == PackageInstaller.STATUS_SUCCESS
    val error = if (ok) "" else intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "Package operation failed (status $status)"
    MatonOSExpoModule.fdroidEventSink?.invoke(operationId, "complete", 100, -1, -1,
      mapOf("ok" to ok, "error" to error, "operationId" to operationId, "kind" to kind))
  }

  companion object {
    const val EXTRA_OPERATION_ID = "org.matonos.flathub.OPERATION_ID"
    const val EXTRA_KIND = "org.matonos.flathub.OPERATION_KIND"
  }
}
