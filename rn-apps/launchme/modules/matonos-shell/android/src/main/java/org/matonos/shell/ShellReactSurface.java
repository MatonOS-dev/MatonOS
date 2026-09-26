package org.matonos.shell;

import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;

import com.facebook.react.ReactHost;
import com.facebook.react.interfaces.TaskInterface;
import com.facebook.react.interfaces.fabric.ReactSurface;

import java.util.concurrent.TimeUnit;

/** Small lifecycle holder for a Fabric surface hosted by an Activity or overlay Service. */
final class ShellReactSurface {
    private final ReactSurface surface;

    private ShellReactSurface(ReactSurface surface) { this.surface = surface; }

    static ShellReactSurface mount(Context context, ReactHost host, String component,
                                   Bundle props, ViewGroup parent, Runnable onStartFailed) {
        ReactSurface surface = host.createSurface(context, component, props);
        View view = surface.getView();
        if (view == null) throw new IllegalStateException("React Native surface has no root view");
        parent.addView(view, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        TaskInterface<Void> task = surface.start();
        Thread watcher = new Thread(() -> {
            try {
                while (!task.waitForCompletion(250, TimeUnit.MILLISECONDS)) { }
                if (task.isFaulted()) new Handler(Looper.getMainLooper()).post(onStartFailed);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            new Handler(Looper.getMainLooper()).post(onStartFailed);
        } catch (RuntimeException | LinkageError failure) {
            new Handler(Looper.getMainLooper()).post(onStartFailed);
        }
        }, "matonos-rn-surface-start");
        watcher.setDaemon(true);
        watcher.start();
        return new ShellReactSurface(surface);
    }

    View getView() { return surface.getView(); }

    void stop() {
        surface.stop();
        surface.clear();
        surface.detach();
    }
}
