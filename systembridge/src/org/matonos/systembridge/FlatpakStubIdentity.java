package org.matonos.systembridge;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/** Pure identity checks shared by reconciliation and the embedded host gate. */
final class FlatpakStubIdentity {
    private static final String PREFIX = "flatpak.";
    private static final String LEGACY_PREFIX = "org.matonos.flatpak.stub.";
    /** One stub per application id: the stable x86_64 branch gets the plain
     * readable name, any other ref a deterministic short suffix. */
    static String packageFor(String ref) throws Exception {
        if(ref==null||ref.length()>1024||!ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid Flatpak reference");
        String[] parts=ref.split("/");
        String id=parts[1];
        boolean plain=parts[2].equals("x86_64")&&parts[3].equals("stable")&&id.length()<=200;
        for(String segment:id.split("\\."))
            if(!segment.matches("[A-Za-z][A-Za-z0-9_]*"))plain=false;
        if(plain)return PREFIX+id;
        // Non-default branch/arch, oversized or invalid ids: keep the name
        // readable where possible, deterministic where not.
        byte[] digest=MessageDigest.getInstance("SHA-256").digest(ref.getBytes(StandardCharsets.UTF_8));
        StringBuilder suffix=new StringBuilder(".x");
        for(int i=0;i<4;i++)suffix.append(String.format(java.util.Locale.ROOT,"%02x",digest[i]&255));
        return PREFIX+id+suffix;
    }
    static boolean isGeneratedStub(String packageName) {
        return packageName!=null&&(packageName.startsWith(PREFIX)||packageName.startsWith(LEGACY_PREFIX));
    }
    static boolean matches(int callerUid,int packageUid,String[] uidPackages,String expectedPackage,
            byte[][] signers,byte[] expectedCertificate) {
        return callerUid==packageUid&&callerUid>=0&&uidPackages!=null&&uidPackages.length==1&&
            expectedPackage!=null&&expectedPackage.equals(uidPackages[0])&&signers!=null&&signers.length==1&&
            signers[0]!=null&&expectedCertificate!=null&&expectedCertificate.length>0&&
            MessageDigest.isEqual(signers[0],expectedCertificate);
    }
}
