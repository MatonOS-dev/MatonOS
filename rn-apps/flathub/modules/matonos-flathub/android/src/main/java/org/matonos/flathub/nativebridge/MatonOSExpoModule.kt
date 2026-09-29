package org.matonos.flathub.nativebridge

import expo.modules.kotlin.Promise
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import org.matonos.client.BridgeMode
import org.matonos.client.MatonOS
import org.matonos.client.MatonosClient
import java.util.concurrent.ConcurrentHashMap

class MatonOSExpoModule : Module() {
  private var client: MatonosClient? = null
  private val listeners = ConcurrentHashMap<String, MatonosClient.EventListener>()

  override fun definition() = ModuleDefinition {
    Name("MatonOSClient")
    Events("matonosAreaEvent")
    OnCreate {
      client = MatonOS.init(requireNotNull(appContext.reactContext), BridgeMode.OPTIONAL)
    }
    OnDestroy {
      client?.let { current ->
        listeners.forEach { (id, listener) ->
          val parts = id.split(":", limit = 3)
          if (parts.size == 3) current.unsubscribe(parts[0], parts[1], listener)
        }
        current.close()
      }
      listeners.clear()
      client = null
    }
    AsyncFunction("checkStartup") { targets: List<String>, channels: List<String>, promise: Promise ->
      val current = requireClient()
      current.checkStartup(targets.toTypedArray(), channels.toTypedArray()) { result ->
        promise.resolve(mapOf("available" to result.available, "reason" to (result.reason ?: ""),
          "apiVersion" to current.bridgeApiVersion, "accessAllowed" to result.available,
          "missingChannels" to if (result.reason?.startsWith("CHANNEL_UNAVAILABLE:") == true) listOf(result.reason.substringAfter(':')) else emptyList<String>()))
      }
    }
    AsyncFunction("call") { target: String, command: String, args: String ->
      val result = requireClient().call(target, command, args)
      mapOf("available" to result.available, "value" to (result.value ?: ""), "reason" to (result.reason ?: ""))
    }
    AsyncFunction("subscribe") { target: String, topic: String, id: String ->
      if (listeners.containsKey(id)) return@AsyncFunction true
      val callback = MatonosClient.EventListener { eventTarget, eventTopic, json ->
        sendEvent("matonosAreaEvent", mapOf("target" to eventTarget, "topic" to eventTopic, "json" to json))
      }
      val result = requireClient().subscribe(target, topic, callback)
      if (result.available) listeners[id] = callback
      result.available
    }
    AsyncFunction("unsubscribe") { id: String ->
      val callback = listeners.remove(id) ?: return@AsyncFunction true
      val parts = id.split(":", limit = 3)
      if (parts.size != 3) return@AsyncFunction false
      requireClient().unsubscribe(parts[0], parts[1], callback).available
    }
  }

  private fun requireClient() = requireNotNull(client) { "MatonOS client is not initialized" }
}
