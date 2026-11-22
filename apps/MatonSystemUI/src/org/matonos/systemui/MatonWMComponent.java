/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import com.android.wm.shell.dagger.WMComponent;
import com.android.wm.shell.dagger.WMShellModule;
import com.android.wm.shell.dagger.WMSingleton;

import dagger.Subcomponent;

@WMSingleton
@Subcomponent(modules = {WMShellModule.class, MatonCaptionModule.class})
public interface MatonWMComponent extends WMComponent {
    @Subcomponent.Builder
    interface Builder extends WMComponent.Builder {
        @Override
        MatonWMComponent build();
    }
}
