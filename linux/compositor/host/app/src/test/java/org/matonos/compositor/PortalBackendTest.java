package org.matonos.compositor;

import java.util.*;
import static org.matonos.compositor.PortalWire.*;

/** Standalone JVM tests: no Android runtime or Gradle dependency required. */
public final class PortalBackendTest {
    static void check(boolean ok){if(!ok)throw new AssertionError();}
    static int serial;
    static Message call(String owner,String iface,String method,String sig,Object... args) {
        Message m=new Message();m.type=1;m.serial=++serial;m.sender=owner;m.destination="org.freedesktop.portal.Desktop";
        m.path=PortalBackend.DESKTOP;m.iface="org.freedesktop.portal."+iface;m.member=method;m.signature=sig;m.body=Arrays.asList(args);
        byte[] wire=encode(m);return decode(wire,m.dbusMessage.getFiledescriptors());
    }
    static List<Object> opts(String token) {return dictionary(Collections.singletonMap("handle_token",new Variant("s",token)));}
    public static void main(String[] args) throws Exception {
        PortalTestPeer.Platform platform=new PortalTestPeer.Platform();PortalBackend b=new PortalBackend(platform);
        Message read=b.dispatch(call(":1.1","Settings","Read","ss","org.freedesktop.appearance","color-scheme")).get(0);
        Variant v=(Variant)decode(encode(read)).body.get(0);check(v.signature.equals("v")&&((Variant)v.value).value.equals(1));
        v=(Variant)b.dispatch(call(":1.1","Settings","ReadOne","ss","org.freedesktop.appearance","color-scheme")).get(0).body.get(0);check(v.signature.equals("u"));
        check(b.dispatch(call(":1.1","Settings","ReadOne","ss","missing","missing")).get(0).error.endsWith("NotFound"));
        Message all=b.dispatch(call(":1.1","Settings","ReadAll","as",Arrays.asList("org.freedesktop.*"))).get(0);
        check(((List<?>)decode(encode(all)).body.get(0)).size()==1);
        check(((List<?>)b.dispatch(call(":1.1","Settings","ReadAll","as",Collections.emptyList())).get(0).body.get(0)).size()==2);
        check(((List<?>)b.dispatch(call(":1.1","Settings","ReadAll","as",Arrays.asList(""))).get(0).body.get(0)).size()==2);
        check(b.dispatch(call(":1.1","Settings","Read","s","bad")).get(0).type==3);
        List<Message> inhibit=b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",8,opts("first")));
        check(inhibit.size()==2&&platform.held&&platform.transitions==1);String handle=(String)inhibit.get(0).body.get(0);
        check(b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",8,opts("first"))).get(0).type==3);
        check(b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",1,opts("logout"))).get(0).type==3);
        check(b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",8,opts("bad/token"))).get(0).type==3);
        b.dispatch(call(":1.2","Inhibit","Inhibit","sua{sv}","",4,opts("other")));check(platform.transitions==1);
        Message close=call(":1.2","Request","Close","");close.path=handle;check(b.dispatch(close).get(0).type==3&&platform.held);
        close.sender=":1.1";check(b.dispatch(close).get(0).type==2&&platform.held);
        b.disconnected(":1.2");check(!platform.held&&platform.transitions==2);
        List<Message> monitor=b.dispatch(call(":1.1","Inhibit","CreateMonitor","sa{sv}","",opts("monitor")));
        check(monitor.size()==3&&!platform.held);Message signal=decode(encode(monitor.get(2)));String session=(String)signal.body.get(0);
        check(b.dispatch(call(":1.2","Inhibit","QueryEndResponse","o",session)).get(0).type==3);
        check(b.dispatch(call(":1.1","Inhibit","QueryEndResponse","o",session)).get(0).type==2);
        check(b.dispatch(call(":1.1","OpenURI","OpenURI","ssa{sv}","","https://example.com",opts("uri"))).get(1).body.get(0).equals(0));
        check(b.dispatch(call(":1.1","OpenURI","OpenURI","ssa{sv}","","fail:",opts("uri"))).get(1).body.get(0).equals(2));
        check(b.dispatch(call(":1.1","OpenURI","OpenFile","sha{sv}","",0,opts("file"))).size()==2);
        check(b.dispatch(call(":1.1","OpenURI","OpenDirectory","sha{sv}","",0,opts("dir"))).size()==2);
        Message sample=call(":1.1","Settings","ReadAll","as",Collections.emptyList());byte[] bytes=encode(sample);
        bytes[4]=(byte)255;try{decode(bytes);throw new AssertionError();}catch(IllegalArgumentException e){}
        Message fdCall=call(":1.1","OpenURI","OpenFile","sha{sv}","",0,opts("fd-check"));
        try {decode(encode(fdCall),Arrays.asList(new org.freedesktop.dbus.FileDescriptor(0),new org.freedesktop.dbus.FileDescriptor(1)));throw new AssertionError();}
        catch(IllegalArgumentException expected){}
        b.close();check(!platform.held);
        for(int i=0;i<255;i++)check(b.dispatch(call(":1.3","Inhibit","Inhibit","sua{sv}","",8,opts("limit_"+i))).get(0).type==2);
        check(b.dispatch(call(":1.3","Inhibit","CreateMonitor","sa{sv}","",opts("limit_monitor"))).get(0).error.endsWith("LimitsExceeded"));
        b.close();check(!platform.held);
        System.out.println("PASS: Java settings, variants/arrays, caller ownership, inhibition lifetime, monitors, OpenURI responses and malformed wire lengths");
    }
}
