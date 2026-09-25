package org.matonos.rncommon;

import com.facebook.react.BaseReactPackage;
import com.facebook.react.bridge.NativeModule;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.model.ReactModuleInfo;
import com.facebook.react.module.model.ReactModuleInfoProvider;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

public final class MatonOSPackage extends BaseReactPackage {
    @Override public NativeModule getModule(String name, ReactApplicationContext context) {
        return MatonOSModule.NAME.equals(name) ? new MatonOSModule(context) : null;
    }

    @Override public ReactModuleInfoProvider getReactModuleInfoProvider() {
        return () -> {
            Map<String, ReactModuleInfo> modules = new HashMap<>();
            modules.put(MatonOSModule.NAME, new ReactModuleInfo(
                    MatonOSModule.NAME, MatonOSModule.class.getName(), false, false, false, true));
            return Collections.unmodifiableMap(modules);
        };
    }
}
