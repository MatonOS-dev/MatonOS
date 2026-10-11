package org.matonos.systembridge;

/** Pure geometry; no caller-supplied bounds or task tokens cross the boundary. */
final class LinuxWindowBounds {
    static boolean validEdges(int edges) {
        return edges >= 0 && (edges & ~15) == 0
                && (edges & 3) != 3 && (edges & 12) != 12;
    }
    static int clamp(int value, int lo, int hi) { return Math.max(lo, Math.min(value, hi)); }
    static int[] apply(int[] start, int[] display, int edges, int dx, int dy,
            int minWidth, int minHeight) {
        int l = start[0], t = start[1], r = start[2], b = start[3];
        if (edges == 0) {
            l = clamp(l + dx, display[0], Math.max(display[0], display[2] - (r - l)));
            t = clamp(t + dy, display[1], Math.max(display[1], display[3] - (b - t)));
            return new int[]{l, t, l + start[2] - start[0], t + start[3] - start[1]};
        }
        minWidth = clamp(minWidth, 1, display[2] - display[0]);
        minHeight = clamp(minHeight, 1, display[3] - display[1]);
        if ((edges & 4) != 0) l = clamp(l + dx, display[0], r - minWidth);
        if ((edges & 8) != 0) r = clamp(r + dx, l + minWidth, display[2]);
        if ((edges & 1) != 0) t = clamp(t + dy, display[1], b - minHeight);
        if ((edges & 2) != 0) b = clamp(b + dy, t + minHeight, display[3]);
        return new int[]{l, t, r, b};
    }
}
