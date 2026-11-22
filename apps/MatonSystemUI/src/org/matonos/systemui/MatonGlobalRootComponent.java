/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import com.android.systemui.dagger.GlobalModule;
import com.android.systemui.dagger.GlobalRootComponent;
import com.android.systemui.dagger.ReferenceSysUIComponent;

import dagger.Component;

import javax.inject.Singleton;

/** Stock reference root with Maton's WM Shell component builder. */
@Singleton
@Component(modules = {GlobalModule.class})
public interface MatonGlobalRootComponent extends GlobalRootComponent {
    @Component.Builder
    interface Builder extends GlobalRootComponent.Builder {
        @Override
        MatonGlobalRootComponent build();
    }

    @Override
    MatonWMComponent.Builder getWMComponentBuilder();

    @Override
    ReferenceSysUIComponent.Builder getSysUIComponent();
}
