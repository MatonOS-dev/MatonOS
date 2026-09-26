package org.matonos.rncommon;

import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReadableArray;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContext;
import com.facebook.react.bridge.WritableArray;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.modules.core.DeviceEventManagerModule;
import com.facebook.react.module.annotations.ReactModule;
import org.matonos.client.BridgeMode;
import org.matonos.client.MatonOS;
import org.matonos.client.MatonosClient;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

@ReactModule(name = MatonOSModule.NAME)
public final class MatonOSModule extends NativeMatonOSSpec {
    public static final String NAME = "MatonOSClient";

    private final MatonosClient client;
    private final Map<String, MatonosClient.EventListener> subscriptions = new ConcurrentHashMap<>();

    MatonOSModule(ReactApplicationContext context) {
        super(context);
        client = MatonOS.init(context, BridgeMode.OPTIONAL);
        client.addAvailabilityListener((available, reason) ->
                context.runOnUiQueueThread(() -> emitAvailability(available, reason)));
    }

    @Override public String getName() { return NAME; }

    @Override public void checkStartup(ReadableArray accessTargets,
                                       ReadableArray requiredChannels,
                                       Promise promise) {
        String[] access = strings(accessTargets);
        String[] channels = strings(requiredChannels);
        client.checkStartup(access, channels, result ->
                promise.resolve(snapshot(result.available, result.reason, result.bridgeApiVersion,
                        result.available, channels, result.reason)));
    }

    @Override public void getBridgeSnapshot(String accessTarget, Promise promise) {
        client.checkStartup(new String[]{accessTarget}, new String[0], result ->
                promise.resolve(snapshot(result.available, result.reason, result.bridgeApiVersion,
                        result.available, new String[0], result.reason)));
    }

    @Override public void call(String target, String command, String jsonArgs, Promise promise) {
        MatonosClient.Result<String> result = client.call(target, command, jsonArgs);
        WritableMap value = Arguments.createMap();
        value.putBoolean("available", result.available);
        value.putString("value", result.available ? result.value : "");
        value.putString("reason", result.available || result.reason == null ? "" : result.reason);
        promise.resolve(value);
    }

    @Override public void subscribe(String target, String topic, String subscriptionId, Promise promise) {
        if (subscriptions.containsKey(subscriptionId)) {
            promise.resolve(true);
            return;
        }
        MatonosClient.EventListener listener = (eventTarget, eventTopic, jsonEvent) -> {
            WritableMap event = Arguments.createMap();
            event.putString("target", eventTarget);
            event.putString("topic", eventTopic);
            event.putString("json", jsonEvent);
            emit("matonosAreaEvent", event);
        };
        MatonosClient.Result<Void> result = client.subscribe(target, topic, listener);
        if (result.available) subscriptions.put(subscriptionId, listener);
        promise.resolve(result.available);
    }

    @Override public void unsubscribe(String subscriptionId, Promise promise) {
        MatonosClient.EventListener listener = subscriptions.remove(subscriptionId);
        if (listener == null) {
            promise.resolve(true);
            return;
        }
        String[] parts = subscriptionId.split(":", 3);
        if (parts.length != 3) {
            promise.resolve(false);
            return;
        }
        promise.resolve(client.unsubscribe(parts[0], parts[1], listener).available);
    }

    @Override public void addListener(String eventName) { }
    @Override public void removeListeners(double count) { }

    private WritableMap snapshot(boolean available, String reason, int apiVersion,
                                 boolean accessAllowed, String[] channels, String failure) {
        WritableMap value = Arguments.createMap();
        value.putBoolean("available", available);
        value.putString("reason", reason == null ? "" : reason);
        value.putInt("apiVersion", apiVersion);
        value.putBoolean("accessAllowed", accessAllowed);
        WritableArray missing = Arguments.createArray();
        if (failure != null && failure.startsWith("CHANNEL_UNAVAILABLE:"))
            missing.pushString(failure.substring("CHANNEL_UNAVAILABLE:".length()));
        value.putArray("missingChannels", missing);
        return value;
    }

    private static String[] strings(ReadableArray values) {
        String[] result = new String[values.size()];
        for (int i = 0; i < result.length; i++) result[i] = values.getString(i);
        return result;
    }

    private void emitAvailability(boolean available, String reason) {
        WritableMap event = Arguments.createMap();
        event.putBoolean("available", available);
        event.putString("reason", reason == null ? "" : reason);
        event.putInt("apiVersion", client.getBridgeApiVersion());
        event.putBoolean("accessAllowed", available);
        event.putArray("missingChannels", Arguments.createArray());
        emit("matonosAvailabilityChanged", event);
    }

    private void emit(String eventName, WritableMap event) {
        ReactContext context = getReactApplicationContext();
        if (!context.hasActiveCatalystInstance()) return;
        context.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter.class)
                .emit(eventName, event);
    }

    @Override public void invalidate() {
        for (Map.Entry<String, MatonosClient.EventListener> entry : subscriptions.entrySet()) {
            String[] parts = entry.getKey().split(":", 3);
            if (parts.length == 3) client.unsubscribe(parts[0], parts[1], entry.getValue());
        }
        subscriptions.clear();
        client.close();
        super.invalidate();
    }
}
