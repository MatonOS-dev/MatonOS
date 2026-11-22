/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import dagger.Module;
import dagger.Provides;

import com.android.wm.shell.dagger.DynamicOverride;
import com.android.wm.shell.dagger.WMSingleton;
import com.android.wm.shell.windowdecor.DesktopCaptionFactory;

import android.content.Context;
import android.os.Handler;

import com.android.wm.shell.shared.annotations.ShellMainThread;

/** Adds MatonOS's optional provider backed caption host to the stock Shell graph. */
@Module
public abstract class MatonCaptionModule {
    @Provides
    @WMSingleton
    @DynamicOverride
    static DesktopCaptionFactory provideDesktopCaptionFactory(Context context,
            @ShellMainThread Handler shellMainHandler) {
        return new CaptionProviderController(context, shellMainHandler);
    }
}
