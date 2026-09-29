package org.matonos.settings.nativebridge

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.drawable.Drawable
import android.util.Log
import android.view.ContextThemeWrapper
import android.widget.ImageView
import expo.modules.kotlin.AppContext
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import expo.modules.kotlin.views.ExpoView

/**
 * <SystemIcon> for our Expo apps: draws a drawable straight from a system
 * package's resources ("android:drawable/ic_battery",
 * "com.android.systemui:drawable/ic_sysbar_back"), tinted, at any size
 * (vector drawables stay sharp). Other packages' resources need package
 * visibility (QUERY_ALL_PACKAGES or a <queries> entry) in the app.
 */
class SystemIconModule : Module() {
  override fun definition() = ModuleDefinition {
    Name("MatonSystemIcon")
    View(SystemIconView::class) {
      Prop("resource") { view: SystemIconView, spec: String? -> view.setResource(spec) }
      Prop("tint") { view: SystemIconView, color: Int? -> view.setTint(color) }
    }
  }
}

class SystemIconView(context: Context, appContext: AppContext) : ExpoView(context, appContext) {
  override val shouldUseAndroidLayout = true
  private val image = ImageView(context).also {
    it.scaleType = ImageView.ScaleType.FIT_CENTER
    addView(it, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
  }

  fun setResource(spec: String?) {
    image.setImageDrawable(spec?.let(::load))
  }

  fun setTint(color: Int?) {
    image.imageTintList = color?.let { ColorStateList.valueOf(it) }
  }

  /** "pkg:type/name" -> drawable, or null when the package/resource is missing. */
  private fun load(spec: String): Drawable? {
    val match = SPEC.matchEntire(spec) ?: return null.also { Log.w(TAG, "Bad resource spec: $spec") }
    val (pkg, type, name) = match.destructured
    return try {
      // Resolve inside the owning package with a device-default theme, so
      // theme attributes in its vector drawables (?attr/...) can resolve.
      // A separate themed wrapper: never change the host view's own theme.
      val pkgContext = if (pkg == "android") context
        else context.createPackageContext(pkg, Context.CONTEXT_IGNORE_SECURITY)
      val themed = ContextThemeWrapper(pkgContext, android.R.style.Theme_DeviceDefault)
      val res = pkgContext.resources
      // SystemUI's nav/status glyphs paint with ?attr/singleToneColor, defined
      // only by its dual-tone styles; without it they draw fully transparent
      // (and tint can't colour transparent pixels).
      val dualTone = res.getIdentifier("DualToneLightTheme", "style", pkg)
      if (dualTone != 0) themed.theme.applyStyle(dualTone, true)
      val id = res.getIdentifier(name, type, pkg)
      if (id == 0) null.also { Log.w(TAG, "No such resource: $spec") }
      else res.getDrawable(id, themed.theme).also {
        Log.d(TAG, "$spec -> 0x${Integer.toHexString(id)} ${res.getResourceName(id)} ${it.javaClass.simpleName}")
      }
    } catch (e: Exception) {
      Log.w(TAG, "Cannot load $spec", e)
      null
    }
  }

  private companion object {
    const val TAG = "MatonSystemIcon"
    val SPEC = Regex("^([A-Za-z0-9_.]+):([a-z]+)/([A-Za-z0-9_]+)$")
  }
}
