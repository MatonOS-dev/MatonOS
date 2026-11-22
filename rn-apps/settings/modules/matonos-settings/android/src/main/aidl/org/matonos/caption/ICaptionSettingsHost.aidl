package org.matonos.caption;

import android.view.SurfaceControlViewHost.SurfacePackage;

oneway interface ICaptionSettingsHost {
    void setSurface(in SurfacePackage p);
    void setPreferredHeight(int heightPx);
}
