package org.matonos.systembridge;

/** Standalone host test: production parser, access policy and consent state. */
public final class RepoManagementTest {
    static void check(boolean value){if(!value)throw new AssertionError();}
    static byte[] file(String extra){return ("[Flatpak Repo]\nTitle=Example\nUrl=https://example.org/repo/\nGPGKey=xgYEAQIDBAU=\n"+extra).getBytes(java.nio.charset.StandardCharsets.UTF_8);}
    static void reject(byte[] bytes) throws Exception {
        try{new FlatpakRepo(bytes);throw new AssertionError("Accepted malformed repo");}catch(IllegalArgumentException expected){}
    }
    public static void main(String[] args) throws Exception {
        FlatpakRepo repo=new FlatpakRepo(file("Homepage=https://example.org/\nComment=Example\nIcon=https://example.org/icon.png\nVersion=1\n"));
        check(repo.host.equals("example.org"));check(repo.name.startsWith("repo-"));
        // Real files (flathub-beta) carry keys we do not use; they are ignored.
        new FlatpakRepo(file("Description=Beta builds\nDefaultBranch=stable\nNoEnumerate=false\n"));
        reject(file("Description=a\nDescription=b\n"));
        reject(file("Bad Key=x\n"));
        // Independent known input: v4 primary packet body 04 01 02 03 04 05.
        java.security.MessageDigest sha=java.security.MessageDigest.getInstance("SHA-1");
        String expected=java.util.HexFormat.of().withUpperCase().formatHex(sha.digest(new byte[]{(byte)0x99,0,6,4,1,2,3,4,5}));
        check(repo.fingerprint.equals(expected));
        String unicode=new String(file(""),java.nio.charset.StandardCharsets.UTF_8).replace("example.org","bücher.example");
        repo=new FlatpakRepo(unicode.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        check(repo.host.equals("xn--bcher-kva.example"));check(!repo.url.contains("bücher"));
        reject(null);reject(new byte[65537]);reject(file("Title=Duplicate\n"));reject(file("[Other]\n"));
        reject("[Flatpak Repo]\nTitle=X\nUrl=https://example.org/\n".getBytes());
        reject(new String(file("")).replace("https://","http://").getBytes());
        reject(new String(file("")).replace("example.org","evil@trusted.org").getBytes());
        reject(new String(file("")).replace("xgYEAQIDBAU=","!!!!").getBytes());
        reject(new String(file("")).replace("xgYEAQIDBAU=","xgYE").getBytes());
        reject(new String(file("")).replace("Example","Spoof\u202Etitle").getBytes());
        for(String command:new String[]{"install_repo","remove_repo"})
            check(LinuxAppStorePolicy.restriction(command).equals("no_install_unknown_sources_globally"));
        for(boolean permission:new boolean[]{false,true})for(boolean admin:new boolean[]{false,true}) {
            boolean allowed=true;try{LinuxAppStorePolicy.enforce(permission,true,admin);}catch(SecurityException denied){allowed=false;}
            check(allowed==(permission&&admin));
        }
        for(RepoDecision.State outcome:new RepoDecision.State[]{RepoDecision.State.CONFIRMED,RepoDecision.State.DENIED,RepoDecision.State.TIMED_OUT,RepoDecision.State.CALLER_DIED}) {
            RepoDecision decision=new RepoDecision();check(decision.finish(outcome));
            check(!decision.finish(RepoDecision.State.CONFIRMED));check(decision.state()==outcome);
            check((decision.state()==RepoDecision.State.CONFIRMED)==(outcome==RepoDecision.State.CONFIRMED));
        }
        RepoDecision racing=new RepoDecision();
        Thread yes=new Thread(()->racing.finish(RepoDecision.State.CONFIRMED));
        Thread death=new Thread(()->racing.finish(RepoDecision.State.CALLER_DIED));
        yes.start();death.start();yes.join();death.join();check(racing.state()!=RepoDecision.State.PENDING);
        System.out.println("PASS repo parser, ASCII host, OpenPGP fingerprint, malformed/oversized/unsigned/HTTP rejection, permission/admin/restrictions and one-shot confirm/deny/timeout/death");
    }
}
