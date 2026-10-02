package org.matonos.systembridge;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/** Pure identity checks shared by reconciliation and the embedded host gate. */
final class FlatpakStubIdentity {
    static String packageFor(String ref) throws Exception {
        if(ref==null||ref.length()>1024||!ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid Flatpak reference");
        byte[] digest=MessageDigest.getInstance("SHA-256").digest(ref.getBytes(StandardCharsets.UTF_8));
        StringBuilder name=new StringBuilder("org.matonos.flatpak.stub.p");
        for(byte b:digest)name.append(String.format(java.util.Locale.ROOT,"%02x",b&255));
        return name.toString();
    }
    static boolean matches(int callerUid,int packageUid,String[] uidPackages,String expectedPackage,
            byte[][] signers,byte[] expectedCertificate) {
        return callerUid==packageUid&&callerUid>=0&&uidPackages!=null&&uidPackages.length==1&&
            expectedPackage!=null&&expectedPackage.equals(uidPackages[0])&&signers!=null&&signers.length==1&&
            signers[0]!=null&&expectedCertificate!=null&&expectedCertificate.length>0&&
            MessageDigest.isEqual(signers[0],expectedCertificate);
    }
}
