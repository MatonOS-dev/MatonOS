package org.matonos.systembridge;

import static org.junit.Assert.*;
import org.junit.Test;

public final class FlatpakStubIdentityTest {
    private static final String REF="app/org.mozilla.firefox/x86_64/stable";
    private static final byte[] CERT={1,2,3};

    @Test public void signedStubWithExactUidAndReferenceIsAccepted() throws Exception {
        String pkg=FlatpakStubIdentity.packageFor(REF);
        assertTrue(FlatpakStubIdentity.matches(10087,10087,new String[]{pkg},pkg,new byte[][]{CERT},CERT));
    }
    @Test public void callerCannotBorrowAnotherApplicationsIdentity() throws Exception {
        String pkg=FlatpakStubIdentity.packageFor(REF);
        String other=FlatpakStubIdentity.packageFor("app/com.example.other/x86_64/stable");
        assertFalse(FlatpakStubIdentity.matches(10088,10087,new String[]{pkg},pkg,new byte[][]{CERT},CERT));
        assertFalse(FlatpakStubIdentity.matches(10087,10087,new String[]{other},pkg,new byte[][]{CERT},CERT));
    }
    @Test public void unsignedImpostorAndChangedSignerAreRejected() throws Exception {
        String pkg=FlatpakStubIdentity.packageFor(REF);
        assertFalse(FlatpakStubIdentity.matches(10087,10087,new String[]{pkg},pkg,new byte[][]{{4}},CERT));
        assertFalse(FlatpakStubIdentity.matches(10087,10087,new String[]{pkg},pkg,null,CERT));
        assertFalse(FlatpakStubIdentity.matches(10087,10087,new String[]{pkg},pkg,new byte[][]{CERT},null));
    }
    @Test public void sharedUidsAndMultipleSignersFailClosed() throws Exception {
        String pkg=FlatpakStubIdentity.packageFor(REF);
        assertFalse(FlatpakStubIdentity.matches(10087,10087,new String[]{pkg,"other"},pkg,new byte[][]{CERT},CERT));
        assertFalse(FlatpakStubIdentity.matches(10087,10087,new String[]{pkg},pkg,new byte[][]{CERT,CERT},CERT));
    }
    @Test public void malformedReferencesAreRejected() throws Exception {
        for(String ref:new String[]{null,"org.mozilla.firefox","runtime/org.mozilla.firefox/x86_64/stable","app/../x86_64/stable/extra"}){
            try {FlatpakStubIdentity.packageFor(ref);fail("Accepted invalid ref");}
            catch(IllegalArgumentException expected){}
        }
    }
    @Test public void branchAndArchitectureHaveDistinctIdentities() throws Exception {
        assertNotEquals(FlatpakStubIdentity.packageFor(REF),FlatpakStubIdentity.packageFor("app/org.mozilla.firefox/x86_64/beta"));
        assertNotEquals(FlatpakStubIdentity.packageFor(REF),FlatpakStubIdentity.packageFor("app/org.mozilla.firefox/aarch64/stable"));
    }
}
