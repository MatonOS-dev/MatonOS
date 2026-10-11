package org.matonos.systembridge;

import static org.junit.Assert.*;
import org.junit.Test;
import java.util.Random;

public final class LinuxWindowBoundsTest {
    private static final int[] START = {100, 100, 500, 400}, DISPLAY = {0, 0, 1000, 800};
    @Test public void moveKeepsSizeAndStaysOnDisplay() {
        assertArrayEquals(new int[]{150,120,550,420}, LinuxWindowBounds.apply(START,DISPLAY,0,50,20,100,80));
        assertArrayEquals(new int[]{600,500,1000,800}, LinuxWindowBounds.apply(START,DISPLAY,0,5000,5000,100,80));
        assertArrayEquals(new int[]{0,0,400,300}, LinuxWindowBounds.apply(START,DISPLAY,0,-5000,-5000,100,80));
    }
    @Test public void leftTopResizeKeepsOppositeEdgesAndMinimumSize() {
        assertArrayEquals(new int[]{400,320,500,400}, LinuxWindowBounds.apply(START,DISPLAY,5,5000,5000,100,80));
        assertArrayEquals(new int[]{0,0,500,400}, LinuxWindowBounds.apply(START,DISPLAY,5,-5000,-5000,100,80));
    }
    @Test public void rejectOppositeEdgesAndUnknownBits() {
        for (int edge : new int[]{0,1,2,4,8,5,6,9,10}) assertTrue(LinuxWindowBounds.validEdges(edge));
        for (int edge : new int[]{-1,3,12,7,11,15,16,256}) assertFalse(LinuxWindowBounds.validEdges(edge));
    }
    @Test public void everyResizeDirectionRespectsBoundsAndOppositeEdges() {
        Random random = new Random(7);
        for (int edge : new int[]{1,2,4,8,5,6,9,10}) for (int i=0;i<1000;i++) {
            int[] b=LinuxWindowBounds.apply(START,DISPLAY,edge,random.nextInt(4001)-2000,random.nextInt(4001)-2000,100,80);
            assertTrue(b[0]>=0 && b[1]>=0 && b[2]<=1000 && b[3]<=800);
            assertTrue(b[2]-b[0]>=100 && b[3]-b[1]>=80);
            if ((edge&4)==0) assertEquals(START[0],b[0]);
            if ((edge&8)==0) assertEquals(START[2],b[2]);
            if ((edge&1)==0) assertEquals(START[1],b[1]);
            if ((edge&2)==0) assertEquals(START[3],b[3]);
        }
    }
}
