/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import android.content.Context;

import com.android.systemui.SystemUIAppComponentFactoryBase;
import com.android.systemui.SystemUIInitializer;

public final class MatonSystemUIAppComponentFactory extends SystemUIAppComponentFactoryBase {
    @Override
    protected SystemUIInitializer createSystemUIInitializer(Context context) {
        return new MatonSystemUIInitializer(context);
    }
}
