package org.matonos.systembridge;

/** Package attribution for package-scoped bridge permissions. */
final class BridgeCallerIdentity {
    private BridgeCallerIdentity() { }

    /**
     * Binder exposes a UID, not the package that made the call. Attribute it only
     * when PackageManager reports exactly one installed package for that UID.
     */
    static String solePackage(String[] packages) {
        if (packages == null || packages.length != 1
                || packages[0] == null || packages[0].isEmpty()) return null;
        return packages[0];
    }

    /** Returns the package only when it is the sole UID member and has a current grant. */
    static String authorizedPackage(String[] packages, String packageWithCurrentGrant) {
        String sole = solePackage(packages);
        return sole != null && sole.equals(packageWithCurrentGrant) ? sole : null;
    }

    static boolean certificateMatches(String currentCertificate, String trustedCertificate) {
        return currentCertificate != null && trustedCertificate != null
                && currentCertificate.equalsIgnoreCase(trustedCertificate);
    }
}
