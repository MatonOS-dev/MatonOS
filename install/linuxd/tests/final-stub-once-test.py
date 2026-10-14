#!/usr/bin/env python3
"""Execute the production bridge install/bind/rollback methods with a fake PM.

The double assigns UID only on PM commit and records replacements. Android
PackageInstaller itself still requires the fresh-image checks in README.md.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
source=(root/'systembridge/src/org/matonos/systembridge/FlatpakStubManager.java').read_text()
def method(start,end):return source[source.index(start):source.index(end)]
methods=method('    private void failInstall(', '    private boolean launcherEnabled(')
with tempfile.TemporaryDirectory(prefix='final-stub-once-',dir='/tmp') as temp:
    work=Path(temp)
    java=work/'BridgeInstallTest.java'
    java.write_text(r'''
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
public class BridgeInstallTest {
    static final String TAG="test";
    static final Set<String> installOperations=new HashSet<>();
    final Map<Integer,Runnable> afterInstall=new HashMap<>(),installFailure=new HashMap<>(),installCleanup=new HashMap<>();
    final Set<String> pending=new HashSet<>();
    final Context context=new Context();final int resultSender=0;
    String failure="";boolean deployed,stub,complete,globalStub;int commits,generations,binds,aborts,existingInstalls;
    final List<String> events=new ArrayList<>();
    static class Log {static void e(String t,String s,Exception e){}static void i(String t,String s){}}
    static class JSONObject {
        final Map<String,String> values=new HashMap<>();
        JSONObject put(String k,Object v){values.put(k,String.valueOf(v));return this;}
        String getString(String k){return values.get(k);}
        String optString(String k){return values.getOrDefault(k,"");}
        public String toString(){return values.toString();}
    }
    class Daemon {
        String call(String command,String args) {
            events.add(command);
            if(command.equals("abort_pending")){deployed=false;aborts++;}
            if(command.equals("install_failed"))complete=true;
            return "{}";
        }
    }
    Daemon daemon(){return new Daemon();}
    JSONObject handoffArgs(String ref,String remote,String operation){return new JSONObject();}
    JSONObject handoff(String command,JSONObject args) throws Exception {
        events.add(command);
        if(command.equals(failure))throw new IOException(command);
        if(command.equals("deploy_pending"))deployed=true;
        if(command.equals("bind_pending")){if(!deployed||!stub)throw new IOException("bad ordering");binds++;deployed=false;}
        if(command.equals("install_complete"))complete=true;
        String output=command.equals("pending_metadata")?"signed-controller-permission":"[Desktop Entry]\nName=Verified fixture\n";
        return new JSONObject().put("output",output);
    }
    String packageFor(String ref){return "flatpak.org.example.App";}
    boolean isOurSignedStub(String pkg){return stub;}
    boolean isGloballySignedStub(String pkg){return globalStub;}
    int verifiedUid(String ref)throws Exception{if(!stub)throw new Exception("no PM UID");return 10120;}
    void showLauncher(String ref){events.add("launcher");}
    byte[] iconFromBase64(String value){return null;}
    byte[] fallbackIcon(){return new byte[]{1};}
    void publishStubSigner(Daemon daemon){}
    class Context {
        PackageManager getPackageManager(){return new PackageManager();}
        File getCacheDir(){return new File(System.getProperty("java.io.tmpdir"));}
    }
    class PackageManager {
        static final int INSTALL_SUCCEEDED=1;
        int installExistingPackage(String p){if(failure.equals("pm-existing"))return -1;stub=true;existingInstalls++;return INSTALL_SUCCEEDED;}
        static class NameNotFoundException extends Exception{}
        Object getPackageInfo(String pkg,int flags)throws NameNotFoundException{if(!stub)throw new NameNotFoundException();return new Object();}
        PackageInstaller getPackageInstaller(){return new PackageInstaller();}
    }
    class PackageInstaller {
        static class SessionParams {
            static final int MODE_FULL_INSTALL=1,USER_ACTION_NOT_REQUIRED=2;
            SessionParams(int mode){}void setAppPackageName(String p){}void setRequireUserAction(int a){}
        }
        int createSession(SessionParams p)throws Exception{if(failure.equals("pm-create"))throw new IOException();return 1;}
        Session openSession(int id){return new Session();}
        void abandonSession(int id){}
        void uninstall(String pkg,int sender){stub=false;events.add("uninstall-stub");}
        class Session implements AutoCloseable {
            OutputStream openWrite(String s,int o,long size)throws Exception{if(failure.equals("pm-write"))throw new IOException();return new ByteArrayOutputStream();}
            void fsync(OutputStream out){}
            void commit(int sender)throws Exception{
                if(failure.equals("pm-commit"))throw new IOException();
                if(!deployed)throw new IOException("stub before deployment");
                commits++;stub=true;events.add("pm-commit");
            }
            public void close(){}
        }
    }
    static BridgeInstallTest active;
    static class StubGenerator {
        static List<String> permissionsForMetadata(String m){if(!m.equals("signed-controller-permission"))throw new AssertionError();return List.of("controllers");}
        static void generate(File work,String ref,String remote,File desktop,byte[] icon,List<String> perms,String pkg,File apk,Object a,Object b)throws Exception{
            if(!active.deployed||active.stub)throw new AssertionError("wrong APK order");
            if(!java.nio.file.Files.readString(desktop.toPath()).contains("Verified fixture"))throw new AssertionError("placeholder name");
            if(!perms.equals(List.of("controllers")))throw new AssertionError("placeholder permissions");
            active.generations++;java.nio.file.Files.write(apk.toPath(),new byte[]{1});
        }
    }
''' +methods+r'''
    void start() {
        try{installStub("app/org.example.App/x86_64/stable","flathub","op");}
        catch(Exception error){failInstall("app/org.example.App/x86_64/stable","flathub","op",error);}
    }
    public static void main(String[] args) {
        for(String failure:List.of("deploy_pending","pending_metadata","pending_desktop","pm-create","pm-write","pm-commit","pm-reject","bind_pending","")) {
            active=new BridgeInstallTest();active.failure=failure;active.start();
            if(active.commits==1) {
                if(failure.equals("pm-reject")){active.stub=false;active.installFailure.remove(1).run();}
                else active.afterInstall.remove(1).run();
            }
            if(failure.isEmpty()) {
                if(active.generations!=1||active.commits!=1||active.binds!=1||!active.stub||!active.complete||active.deployed)throw new AssertionError("success");
                int before=active.commits;active.start(); // existing APK: no refresh
                if(active.commits!=before||active.generations!=1||active.binds!=2)throw new AssertionError("refresh");
            } else if(active.stub||active.deployed||!active.complete)throw new AssertionError("failure cleanup: "+failure);
            System.out.println("PASS final APK once: "+(failure.isEmpty()?"success and no-refresh update":failure));
        }
        active=new BridgeInstallTest();active.globalStub=true;active.start();
        if(active.commits!=0||active.generations!=0||active.existingInstalls!=1||active.binds!=1||!active.complete)throw new AssertionError("cross-user APK replacement");
        active=new BridgeInstallTest();active.globalStub=true;active.failure="pm-existing";active.start();
        if(active.stub||active.deployed||!active.complete)throw new AssertionError("cross-user failure cleanup");
        System.out.println("PASS other user reuses final APK; install-existing failure cleans deployment");
    }
}
''')
    subprocess.run(['/mnt/data/aosp/prebuilts/jdk/jdk21/linux-x86/bin/javac',str(java)],check=True)
    subprocess.run(['/mnt/data/aosp/prebuilts/jdk/jdk21/linux-x86/bin/java','-ea',f'-Djava.io.tmpdir={work}','-cp',str(work),'BridgeInstallTest'],check=True)
