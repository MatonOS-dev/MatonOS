package org.matonos.rncommon

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.drawable.Drawable
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
      Prop("resource") { view: SystemIconView, spec: String -> view.setResource(spec) }
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

  fun setResource(spec: String) {
    image.setImageDrawable(load(spec))
  }

  fun setTint(color: Int?) {
    image.imageTintList = color?.let { ColorStateList.valueOf(it) }
  }

  /** "pkg:type/name" -> drawable, or null when the package/resource is missing. */
  private fun load(spec: String): Drawable? {
    val match = SPEC.matchEntire(spec) ?: return null
    val (pkg, type, name) = match.destructured
    return try {
      // Resolve inside the owning package with a device-default theme, so
      // theme attributes in its vector drawables (?attr/...) can resolve.
      val pkgContext = if (pkg == "android") context
        else context.createPackageContext(pkg, Context.CONTEXT_IGNORE_SECURITY)
      pkgContext.setTheme(android.R.style.Theme_DeviceDefault)
      val res = pkgContext.resources
      val id = res.getIdentifier(name, type, pkg)
      if (id == 0) null else res.getDrawable(id, pkgContext.theme)
    } catch (e: Exception) {
      null
    }
  }

  private companion object {
    val SPEC = Regex("^([A-Za-z0-9_.]+):([a-z]+)/([A-Za-z0-9_]+)$")
  }
}
