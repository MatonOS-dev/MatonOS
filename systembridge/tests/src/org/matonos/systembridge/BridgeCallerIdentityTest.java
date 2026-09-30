package org.matonos.systembridge;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class BridgeCallerIdentityTest {
    @Test
    public void solePackageCanBeAttributed() {
        assertEquals("com.example.a", BridgeCallerIdentity.solePackage(
                new String[] {"com.example.a"}));
    }

    @Test
    public void sharedUidCannotBorrowTrustedPackageIdentity() {
        // The caller's Binder UID contains A and B. Since Binder cannot identify
        // which package originated the call, neither package is attributable.
        assertNull(BridgeCallerIdentity.authorizedPackage(
                new String[] {"com.example.a", "com.example.b"}, "com.example.a"));
    }

    @Test
    public void removingSharedPackageAllowsOnlyTheRemainingTrustedPackage() {
        String[] beforeRemoval = {"com.example.a", "com.example.b"};
        String[] afterRemoval = {"com.example.a"};
        assertNull(BridgeCallerIdentity.authorizedPackage(beforeRemoval, "com.example.a"));
        assertEquals("com.example.a", BridgeCallerIdentity.authorizedPackage(afterRemoval,
                "com.example.a"));
    }

    @Test
    public void revokingTrustRemovesTheAuthorizationCandidate() {
        // The caller resolves the current trust record for each request.
        assertNull(BridgeCallerIdentity.authorizedPackage(
                new String[] {"com.example.a"}, null));
    }

    @Test
    public void signerUpdateDoesNotReuseTheOldCertificateGrant() {
        assertTrue(BridgeCallerIdentity.certificateMatches("aabb", "AABB"));
        assertFalse(BridgeCallerIdentity.certificateMatches("ccdd", "aabb"));
        // A changed signer cannot reuse the prior package trust record.
        assertNull(BridgeCallerIdentity.authorizedPackage(
                new String[] {"com.example.a"}, null));
    }

    @Test
    public void nullOrEmptyPackageListsFailClosed() {
        assertNull(BridgeCallerIdentity.solePackage(null));
        assertNull(BridgeCallerIdentity.solePackage(new String[0]));
        assertNull(BridgeCallerIdentity.solePackage(new String[] {""}));
    }
}
