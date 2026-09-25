package org.matonos.shell;

import com.facebook.react.BaseReactPackage;
import com.facebook.react.bridge.NativeModule;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.model.ReactModuleInfo;
import com.facebook.react.module.model.ReactModuleInfoProvider;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

public final class MatonShellPackage extends BaseReactPackage {
    @Override public NativeModule getModule(String name, ReactApplicationContext context) {
        return MatonShellModule.NAME.equals(name) ? new MatonShellModule(context) : null;
    }

    @Override public ReactModuleInfoProvider getReactModuleInfoProvider() {
        return () -> {
            Map<String, ReactModuleInfo> modules = new HashMap<>();
            modules.put(MatonShellModule.NAME, new ReactModuleInfo(
                    MatonShellModule.NAME, MatonShellModule.class.getName(), false, false, false, true));
            return Collections.unmodifiableMap(modules);
        };
    }
}
