package org.matonos.settings

import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

class CaptionSettingsSlotModule : Module() {
  override fun definition() = ModuleDefinition {
    Name("MatonCaptionSettingsSlot")
    View(CaptionSettingsSlotView::class)
  }
}
