package org.matonos.settings

import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

class EmbeddedSettingsSlotModule : Module() {
  override fun definition() = ModuleDefinition {
    Name("MatonEmbeddedSettingsSlot")
    View(EmbeddedSettingsSlotView::class) {
      Prop("componentName") { view: EmbeddedSettingsSlotView, value: String ->
        view.setComponentName(value)
      }
    }
  }
}
