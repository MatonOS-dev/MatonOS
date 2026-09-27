package org.matonos.shelf

import android.app.Notification
import android.content.Context
import android.util.Log
import android.widget.RemoteViews
import expo.modules.kotlin.AppContext
import expo.modules.kotlin.views.ExpoView

/**
 * <NotificationContent notificationKey=... expanded=...>: Android's own rendering
 * of a notification's content (the app's RemoteViews via Notification.Builder),
 * for apps with custom layouts (media players, ...). The card chrome around it
 * (icon, app name, dismiss, ...) stays in React Native.
 */
class NotificationContentView(context: Context, appContext: AppContext) : ExpoView(context, appContext) {
  override val shouldUseAndroidLayout = true
  private var key: String = ""
  private var expanded = false

  fun setKey(value: String?) { key = value ?: ""; render() }
  fun setExpanded(value: Boolean?) { expanded = value ?: false; render() }

  private fun render() {
    removeAllViews()
    val sbn = ShelfNotificationListener.findForView(key) ?: return
    try {
      val pkgContext = context.createPackageContext(sbn.packageName, Context.CONTEXT_RESTRICTED)
      val builder = Notification.Builder.recoverBuilder(pkgContext, sbn.notification)
      val remote: RemoteViews? = if (expanded) builder.createBigContentView() ?: builder.createContentView()
        else builder.createContentView()
      val view = remote?.apply(context, this) ?: return
      addView(view, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
    } catch (e: Exception) {
      Log.w("ShelfNotifications", "Cannot render notification $key", e)
    }
  }
}
