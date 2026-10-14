#!/usr/bin/env python3
"""Execute production bridge consent/removal methods with PM/Binder doubles.

This verifies wiring beyond the pure consent state; real Activity/PM remains
an enforcing fresh-image check. No device commands are used.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
source=(root/'systembridge/src/org/matonos/systembridge/SystemBridgeService.java').read_text()
methods=source[source.index('    void decideRepo('):source.index('    private final ILinuxAppStore.Stub storeBinder=')]
with tempfile.TemporaryDirectory(prefix='repo-confirmation-',dir='/tmp') as tmp:
    work=Path(tmp)
    stubs={
    'android/os/RemoteException.java':'package android.os; public class RemoteException extends Exception {}',
    'android/os/UserHandle.java':'package android.os; public class UserHandle {public int id;public static UserHandle of(int id){UserHandle u=new UserHandle();u.id=id;return u;}}',
    'android/os/SystemClock.java':'package android.os; public class SystemClock {public static long elapsedRealtime(){return System.nanoTime()/1000000;}}',
    'android/os/IBinder.java':'package android.os; public interface IBinder {interface DeathRecipient{void binderDied();} boolean isBinderAlive();boolean unlinkToDeath(DeathRecipient d,int flags);}',
    'android/content/pm/PackageManager.java':'package android.content.pm; public class PackageManager {public static class NameNotFoundException extends Exception{} public Object getApplicationInfo(String p,int flags)throws NameNotFoundException{throw new NameNotFoundException();}}',
    }
    for name,text in stubs.items():
        p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    java=work/'RepoBridgeTest.java'
    java.write_text(r'''
package org.matonos.systembridge;
import java.util.*;
import android.os.RemoteException;
import android.os.UserHandle;
import android.content.pm.PackageManager;
public class RepoBridgeTest {
    static final Map<String,RepoRequest> repoRequests=new HashMap<>();
    static final Map<String,String> repoResults=new HashMap<>();
    final java.util.concurrent.Executor repoWorker=Runnable::run;
    final List<String> calls=new ArrayList<>();boolean blocked,changed,removeFailure;
    final Owner flatpakStubManager=new Owner();
    class Owner {int user;Owner forUser(int u){user=u;return this;}void uninstall(String ref,boolean data){calls.add("uninstall:"+user+":"+ref);}}
    static class FlatpakStubIdentity {static String packageFor(String ref){return "flatpak.org.example.App";}}
    static class JSONObject {
        Map<String,Object> fields=new LinkedHashMap<>();JSONObject put(String k,Object v){fields.put(k,v);return this;}
        public String toString(){return fields.toString();}
    }
    static class Context {PackageManager getPackageManager(){return new PackageManager();}}
    Context createContextAsUser(UserHandle user,int flags){return new Context();}
    void enforceRepoUser(int user,String command){if(blocked)throw new SecurityException("restriction/admin changed");}
    Map<Integer,Map<String,String>> repoApps(String remote){return changed?Collections.emptyMap():apps();}
    JSONObject storeReply(String command,JSONObject args){calls.add(command);if(removeFailure && command.equals("remove_repo"))throw new IllegalStateException("deployment still held");return new JSONObject();}
    static Map<Integer,Map<String,String>> apps(){Map<Integer,Map<String,String>> result=new HashMap<>();result.put(0,Map.of("org.example.App","app/org.example.App/x86_64/stable"));result.put(10,Map.of("org.example.App","app/org.example.App/x86_64/stable"));return result;}
    static class Repo {String canonical="canonical";}
    static class Caller implements android.os.IBinder {
        boolean alive=true;int finishes;boolean ok;String error;
        android.os.IBinder asBinder(){return this;}
        public boolean isBinderAlive(){return alive;}
        public boolean unlinkToDeath(android.os.IBinder.DeathRecipient death,int flags){return true;}
        void onFinished(String operation,boolean ok,String error)throws RemoteException{finishes++;this.ok=ok;this.error=error;}
    }
    static class RepoRequest {
        final String operation=UUID.randomUUID().toString();final RepoDecision decision=new RepoDecision();
        final int user=0;String remote="example";Repo repo;Caller caller=new Caller();boolean removing;
        Map<Integer,Map<String,String>> apps=RepoBridgeTest.apps();android.os.IBinder.DeathRecipient death;
        RepoRequest(boolean removing){this.removing=removing;if(!removing)repo=new Repo();}
    }
''' + methods + r'''
    static void check(boolean value){if(!value)throw new AssertionError();}
    static RepoRequest request(boolean removing){RepoRequest r=new RepoRequest(removing);repoRequests.put(r.operation,r);return r;}
    public static void main(String[] args) {
        for(RepoDecision.State state:new RepoDecision.State[]{RepoDecision.State.DENIED,RepoDecision.State.TIMED_OUT,RepoDecision.State.CALLER_DIED}) {
            RepoBridgeTest bridge=new RepoBridgeTest();RepoRequest r=request(false);bridge.decideRepo(r.operation,state);
            check(bridge.calls.isEmpty());check(r.caller.finishes==1 && !r.caller.ok);check(!repoRequests.containsKey(r.operation));
            bridge.decideRepo(r.operation,RepoDecision.State.CONFIRMED);check(bridge.calls.isEmpty() && r.caller.finishes==1);
        }
        RepoBridgeTest add=new RepoBridgeTest();RepoRequest r=request(false);add.decideRepo(r.operation,RepoDecision.State.CONFIRMED);
        check(add.calls.equals(List.of("install_repo")) && r.caller.ok);
        RepoBridgeTest remove=new RepoBridgeTest();r=request(true);remove.decideRepo(r.operation,RepoDecision.State.CONFIRMED);
        check(remove.calls.size()==4 && remove.calls.get(0).startsWith("uninstall:") && remove.calls.get(1).startsWith("uninstall:"));
        check(remove.calls.subList(2,4).equals(List.of("reconcile_deployments","remove_repo")) && r.caller.ok);
        for(int scenario=0;scenario<3;scenario++) {
            RepoBridgeTest denied=new RepoBridgeTest();r=request(true);
            if(scenario==0)denied.blocked=true;if(scenario==1)denied.changed=true;if(scenario==2)r.caller.alive=false;
            denied.decideRepo(r.operation,RepoDecision.State.CONFIRMED);check(denied.calls.isEmpty() && !r.caller.ok);
        }
        RepoBridgeTest held=new RepoBridgeTest();held.removeFailure=true;r=request(true);held.decideRepo(r.operation,RepoDecision.State.CONFIRMED);
        check(!r.caller.ok && r.caller.error.contains("held"));
        System.out.println("PASS production bridge confirm/deny/timeout/death, recheck admin/restriction/caller/inventory, all-user uninstall before remote removal, held-deployment failure");
    }
}
''')
    subprocess.run(['javac','-d',str(work),*map(str,work.rglob('*.java')),str(root/'systembridge/src/org/matonos/systembridge/RepoDecision.java')],check=True)
    subprocess.run(['java','-cp',str(work),'org.matonos.systembridge.RepoBridgeTest'],check=True)
