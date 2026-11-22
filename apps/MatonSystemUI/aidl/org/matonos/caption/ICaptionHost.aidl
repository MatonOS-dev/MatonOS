package org.matonos.caption;

import android.graphics.Rect;
import android.view.SurfaceControlViewHost.SurfacePackage;

/** SystemUI callbacks available to a caption provider. */
oneway interface ICaptionHost {
    void setSurface(int taskId, in SurfacePackage p);
    void setInteractiveRegions(int taskId, in Rect[] regions);
    void close(int taskId);
    void toggleMaximize(int taskId);
    void minimize(int taskId);
    void openAppMenu(int taskId, in Rect anchor);
}
