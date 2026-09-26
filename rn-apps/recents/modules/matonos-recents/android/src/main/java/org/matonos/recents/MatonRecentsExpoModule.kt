package org.matonos.recents

import android.content.Intent
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

class MatonRecentsExpoModule : Module() {
  override fun definition() = ModuleDefinition {
    Name("MatonRecents")
    Function("goHome") {
      val context = requireNotNull(appContext.reactContext)
      context.startActivity(Intent(Intent.ACTION_MAIN)
        .addCategory(Intent.CATEGORY_HOME)
        .addCategory(Intent.CATEGORY_DEFAULT)
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
    Function("reportSurfaceFailure") { error: String ->
      val context = requireNotNull(appContext.reactContext)
      context.sendBroadcast(Intent(ACTION_SURFACE_FAILED).setPackage(context.packageName)
        .putExtra("error", error))
    }
  }

  companion object {
    const val ACTION_SURFACE_FAILED = "org.matonos.recents.RN_SURFACE_FAILED"
  }
}
