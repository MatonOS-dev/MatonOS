package org.matonos.rncommon

import expo.modules.kotlin.Promise
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import org.matonos.client.BridgeMode
import org.matonos.client.MatonOS
import org.matonos.client.MatonosClient
import java.util.concurrent.ConcurrentHashMap

/** Expo adapter over buildinfra/client. All bridge semantics remain in that shared Java client. */
class MatonOSExpoModule : Module() {
  private var client: MatonosClient? = null
  private val listeners = ConcurrentHashMap<String, MatonosClient.EventListener>()
  private var availabilityListener: MatonosClient.AvailabilityListener? = null

  override fun definition() = ModuleDefinition {
    Name("MatonOSClient")
    Events("matonosAreaEvent", "matonosAvailabilityChanged")

    OnCreate {
      val context = requireNotNull(appContext.reactContext)
      val current = MatonOS.init(context, BridgeMode.OPTIONAL)
      client = current
      val callback = MatonosClient.AvailabilityListener { available, reason ->
        sendEvent("matonosAvailabilityChanged", snapshot(current, available, reason, emptyList()))
      }
      availabilityListener = callback
      current.addAvailabilityListener(callback)
    }
    OnDestroy {
      client?.let { current ->
        availabilityListener?.let(current::removeAvailabilityListener)
        listeners.forEach { (id, listener) ->
          val parts = id.split(":", limit = 3)
          if (parts.size == 3) current.unsubscribe(parts[0], parts[1], listener)
        }
        current.close()
      }
      listeners.clear()
      client = null
    }

    AsyncFunction("checkStartup") { accessTargets: List<String>, requiredChannels: List<String>, promise: Promise ->
      val current = requireClient()
      current.checkStartup(accessTargets.toTypedArray(), requiredChannels.toTypedArray()) { result ->
        promise.resolve(snapshot(current, result.available, result.reason, requiredChannels))
      }
    }
    AsyncFunction("getBridgeSnapshot") { accessTarget: String ->
      val current = requireClient()
      mapOf(
        "available" to current.isAvailable(), "reason" to (current.reason ?: ""),
        "apiVersion" to current.bridgeApiVersion, "accessAllowed" to current.isTargetAllowed(accessTarget),
        "missingChannels" to emptyList<String>(),
      )
    }
    AsyncFunction("call") { target: String, command: String, jsonArgs: String ->
      val result = requireClient().call(target, command, jsonArgs)
      mapOf("available" to result.available, "value" to (result.value ?: ""), "reason" to (result.reason ?: ""))
    }
    AsyncFunction("injectBackKey") {
      val result = requireClient().injectBackKey()
      mapOf("available" to result.available, "value" to (if (result.value == true) "true" else "false"), "reason" to (result.reason ?: ""))
    }
    AsyncFunction("subscribe") { target: String, topic: String, subscriptionId: String ->
      val current = requireClient()
      if (listeners.containsKey(subscriptionId)) return@AsyncFunction true
      val listener = MatonosClient.EventListener { eventTarget, eventTopic, json ->
        sendEvent("matonosAreaEvent", mapOf("target" to eventTarget, "topic" to eventTopic, "json" to json))
      }
      val result = current.subscribe(target, topic, listener)
      if (result.available) listeners[subscriptionId] = listener
      result.available
    }
    AsyncFunction("unsubscribe") { subscriptionId: String ->
      val listener = listeners.remove(subscriptionId) ?: return@AsyncFunction true
      val parts = subscriptionId.split(":", limit = 3)
      if (parts.size != 3) return@AsyncFunction false
      requireClient().unsubscribe(parts[0], parts[1], listener).available
    }
  }

  private fun requireClient() = requireNotNull(client) { "MatonOS client is not initialized" }

  private fun snapshot(current: MatonosClient, available: Boolean, reason: String?, channels: List<String>) = mapOf(
    "available" to available,
    "reason" to (reason ?: ""),
    "apiVersion" to current.bridgeApiVersion,
    "accessAllowed" to available,
    "missingChannels" to if (reason?.startsWith("CHANNEL_UNAVAILABLE:") == true) listOf(reason.substringAfter(':')) else emptyList<String>(),
  )
}
