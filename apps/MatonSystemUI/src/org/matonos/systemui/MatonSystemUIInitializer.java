/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import android.content.Context;

import com.android.systemui.SystemUIInitializer;
import com.android.systemui.dagger.GlobalRootComponent;

public final class MatonSystemUIInitializer extends SystemUIInitializer {
    private final VolumeBoostController mVolumeBoostController;

    public MatonSystemUIInitializer(Context context) {
        super(context);
        mVolumeBoostController = new VolumeBoostController(context);
    }

    @Override
    protected GlobalRootComponent.Builder getGlobalRootComponentBuilder() {
        return DaggerMatonGlobalRootComponent.builder();
    }
}
