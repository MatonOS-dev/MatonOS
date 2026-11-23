/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import android.app.Application;
import android.content.Context;
import android.os.Process;

import com.android.systemui.SystemUIInitializer;
import com.android.systemui.dagger.GlobalRootComponent;

public final class MatonSystemUIInitializer extends SystemUIInitializer {
    private final VolumeBoostController mVolumeBoostController;

    public MatonSystemUIInitializer(Context context) {
        super(context);
        // One output-mix effect for the whole device: only the main SystemUI process of user 0
        // owns it (secondary SystemUI processes and users would stack the gain).
        boolean owner = Process.myUserHandle().isSystem()
                && context.getPackageName().equals(Application.getProcessName());
        mVolumeBoostController = owner ? new VolumeBoostController(context) : null;
    }

    @Override
    protected GlobalRootComponent.Builder getGlobalRootComponentBuilder() {
        return DaggerMatonGlobalRootComponent.builder();
    }
}
