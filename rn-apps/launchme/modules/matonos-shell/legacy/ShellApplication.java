package org.matonos.shell;

import android.app.Application;

import com.facebook.react.PackageList;
import com.facebook.react.ReactApplication;
import com.facebook.react.ReactHost;
import com.facebook.react.ReactNativeHost;
import com.facebook.react.ReactPackage;
import com.facebook.react.defaults.DefaultNewArchitectureEntryPoint;
import com.facebook.react.defaults.DefaultReactHost;
import com.facebook.react.defaults.DefaultReactNativeHost;
import com.facebook.react.soloader.OpenSourceMergedSoMapping;
import com.facebook.soloader.SoLoader;
import com.google.android.material.color.DynamicColors;

import java.util.List;

/** Owns the single Hermes host shared by HOME, panels and the shelf surface. */
public final class ShellApplication extends Application implements ReactApplication {
    public static final String ACTION_RN_SURFACE_FAILED = "org.matonos.shell.RN_SURFACE_FAILED";

    private final ReactNativeHost reactNativeHost = new DefaultReactNativeHost(this) {
        @Override public List<ReactPackage> getPackages() {
            List<ReactPackage> packages = new PackageList(this).getPackages();
            packages.add(new MatonShellPackage());
            return packages;
        }

        @Override public String getJSMainModuleName() { return "index"; }
        @Override public boolean getUseDeveloperSupport() { return BuildConfig.DEBUG; }
        @Override public boolean isNewArchEnabled() { return BuildConfig.IS_NEW_ARCHITECTURE_ENABLED; }
        @Override public boolean isHermesEnabled() { return BuildConfig.IS_HERMES_ENABLED; }
    };

    @Override public ReactNativeHost getReactNativeHost() { return reactNativeHost; }

    @Override public ReactHost getReactHost() {
        return DefaultReactHost.getDefaultReactHost(getApplicationContext(), reactNativeHost, null);
    }

    @Override public void onCreate() {
        super.onCreate();
        try { SoLoader.init(this, OpenSourceMergedSoMapping.INSTANCE); }
        catch (java.io.IOException error) { throw new IllegalStateException("SoLoader initialization failed", error); }
        if (BuildConfig.IS_NEW_ARCHITECTURE_ENABLED) DefaultNewArchitectureEntryPoint.load();
        DynamicColors.applyToActivitiesIfAvailable(this);
    }
}
