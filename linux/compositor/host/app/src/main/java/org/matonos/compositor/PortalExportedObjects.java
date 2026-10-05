package org.matonos.compositor;

import org.freedesktop.dbus.DBusPath;
import org.freedesktop.dbus.annotations.DBusInterfaceName;
import org.freedesktop.dbus.annotations.DBusMemberName;
import org.freedesktop.dbus.exceptions.DBusException;
import org.freedesktop.dbus.exceptions.DBusExecutionException;
import org.freedesktop.dbus.interfaces.DBusInterface;
import org.freedesktop.dbus.interfaces.Introspectable;
import org.freedesktop.dbus.messages.DBusSignal;
import org.freedesktop.dbus.types.UInt32;
import org.freedesktop.dbus.types.Variant;
import org.freedesktop.dbus.connections.impl.DirectConnection;
import org.freedesktop.dbus.connections.base.AbstractConnectionBase;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import static org.matonos.compositor.PortalWire.*;

/** dbus-java exported portal interfaces. PortalBackend retains the portal state and policy. */
final class PortalExportedObjects {
    @DBusInterfaceName("org.freedesktop.portal.Settings")
    public interface Settings extends DBusInterface {
        @DBusMemberName("Read") Variant<?> Read(String namespace,String key);
        @DBusMemberName("ReadOne") Variant<?> ReadOne(String namespace,String key);
        @DBusMemberName("ReadAll") Map<String,Map<String,Variant<?>>> ReadAll(List<String> namespaces);
    }
    @DBusInterfaceName("org.freedesktop.portal.Inhibit")
    public interface Inhibit extends DBusInterface {
        @DBusMemberName("Inhibit") DBusPath Inhibit(String appId,UInt32 flags,Map<String,Variant<?>> options);
        @DBusMemberName("CreateMonitor") DBusPath CreateMonitor(String appId,Map<String,Variant<?>> options);
        @DBusMemberName("QueryEndResponse") void QueryEndResponse(DBusPath session);
    }
    @DBusInterfaceName("org.freedesktop.portal.OpenURI")
    public interface OpenURI extends DBusInterface {
        @DBusMemberName("OpenURI") DBusPath OpenURI(String parent,String uri,Map<String,Variant<?>> options);
        @DBusMemberName("OpenFile") DBusPath OpenFile(String parent,org.freedesktop.dbus.FileDescriptor fd,Map<String,Variant<?>> options);
        @DBusMemberName("OpenDirectory") DBusPath OpenDirectory(String parent,org.freedesktop.dbus.FileDescriptor fd,Map<String,Variant<?>> options);
    }
    @DBusInterfaceName("org.freedesktop.portal.Request")
    public interface Request extends DBusInterface { @DBusMemberName("Close") void Close(); }
    @DBusInterfaceName("org.freedesktop.portal.Session")
    public interface Session extends DBusInterface { @DBusMemberName("Close") void Close(); }
    @DBusInterfaceName("org.freedesktop.DBus.Properties")
    public interface Properties extends DBusInterface {
        @DBusMemberName("Get") Variant<?> Get(String iface,String property);
        @DBusMemberName("GetAll") Map<String,Variant<?>> GetAll(String iface);
        @DBusMemberName("Set") void Set(String iface,String property,Variant<?> value);
    }

    @DBusInterfaceName("org.matonos.PortalBackend")
    public static final class ClientClosed extends DBusSignal {
        public final String owner;
        public ClientClosed(String path,String owner)throws DBusException{super(path,owner);this.owner=owner;}
    }

    private final DirectConnection connection;
    private final PortalBackend backend;
    private final PortalIntrospection introspection;

    PortalExportedObjects(DirectConnection connection,PortalBackend backend){this.connection=connection;this.backend=backend;introspection=new PortalIntrospection();}
    void export()throws DBusException{
        connection.exportObject(PortalBackend.DESKTOP,new Desktop());
        connection.addFallback(PortalBackend.DESKTOP+"/request",new RequestObject());
        connection.addFallback(PortalBackend.DESKTOP+"/session",new SessionObject());
    }

    private List<Message> dispatch(String iface,String member,String signature,Object... args){
        org.freedesktop.dbus.DBusCallInfo info=AbstractConnectionBase.getCallInfo();
        if(info==null||info.getSource()==null)throw dbusError("org.freedesktop.DBus.Error.AccessDenied","Missing authenticated caller");
        try {
            Message call=new Message();call.type=1;call.serial=1;call.path=info.getObjectPath();call.iface=iface;call.member=member;call.signature=signature;call.sender=info.getSource();
            for(int i=0;i<args.length;i++)call.body.add(PortalWire.fromDbusValue(signaturePart(signature,i),args[i]));
            List<Message> replies=backend.dispatch(call);
            if(replies.isEmpty())throw dbusError("org.freedesktop.DBus.Error.Failed","Portal returned no method result");
            Message result=replies.get(0);
            if(result.type==3){String detail=result.body.isEmpty()?"Portal operation failed":String.valueOf(result.body.get(0));throw dbusError(result.error,detail);}
            for(int i=1;i<replies.size();i++)if(replies.get(i).type==4)connection.sendMessage(PortalWire.toDbusMessage(replies.get(i)));
            return replies;
        } finally {
            for(Object arg:args)if(arg instanceof org.freedesktop.dbus.FileDescriptor)
                MatonLocalTransportProvider.releaseDescriptor(((org.freedesktop.dbus.FileDescriptor)arg).getIntFileDescriptor());
        }
    }

    private Object result(String iface,String member,String sig,String replySig,Object... args){
        List<Message> messages=dispatch(iface,member,sig,args);Message response=messages.get(0);
        return response.body.isEmpty()?null:PortalWire.toDbusValue(replySig,response.body.get(0));
    }
    private static DBusExecutionException dbusError(String name,String text){DBusExecutionException error=new DBusExecutionException(text);error.setType(name);return error;}
    private static Map<String,Object> portalMap(Map<String,Variant<?>> values){Map<String,Object> out=new LinkedHashMap<>();if(values!=null)out.putAll(values);return out;}

    private abstract class BaseObject implements Introspectable,Properties {
        private final String objectPath;private final boolean desktop,session;
        BaseObject(String objectPath,boolean desktop,boolean session){this.objectPath=objectPath;this.desktop=desktop;this.session=session;}
        @Override public String getObjectPath(){return objectPath;}
        @Override public String Introspect(){return PortalIntrospection.xml(desktop,session);}
        @Override public Variant<?> Get(String iface,String property){return (Variant<?>)result("org.freedesktop.DBus.Properties","Get","ss","v",iface,property);}
        @Override @SuppressWarnings("unchecked") public Map<String,Variant<?>> GetAll(String iface){return (Map<String,Variant<?>>)result("org.freedesktop.DBus.Properties","GetAll","s","a{sv}",iface);}
        @Override public void Set(String iface,String property,Variant<?> value){result("org.freedesktop.DBus.Properties","Set","ssv","",iface,property,value);}
    }
    private final class Desktop extends BaseObject implements Settings,Inhibit,OpenURI {
        Desktop(){super(PortalBackend.DESKTOP,true,false);}
        @Override public Variant<?> Read(String ns,String key){return (Variant<?>)result("org.freedesktop.portal.Settings","Read","ss","v",ns,key);}
        @Override public Variant<?> ReadOne(String ns,String key){return (Variant<?>)result("org.freedesktop.portal.Settings","ReadOne","ss","v",ns,key);}
        @Override @SuppressWarnings("unchecked") public Map<String,Map<String,Variant<?>>> ReadAll(List<String> namespaces){return (Map<String,Map<String,Variant<?>>>)result("org.freedesktop.portal.Settings","ReadAll","as","a{sa{sv}}",namespaces);}
        @Override public DBusPath Inhibit(String appId,UInt32 flags,Map<String,Variant<?>> options){return (DBusPath)result("org.freedesktop.portal.Inhibit","Inhibit","sua{sv}","o",appId,flags,portalMap(options));}
        @Override public DBusPath CreateMonitor(String appId,Map<String,Variant<?>> options){return (DBusPath)result("org.freedesktop.portal.Inhibit","CreateMonitor","sa{sv}","o",appId,portalMap(options));}
        @Override public void QueryEndResponse(DBusPath session){result("org.freedesktop.portal.Inhibit","QueryEndResponse","o","",session);}
        @Override public DBusPath OpenURI(String parent,String uri,Map<String,Variant<?>> options){return (DBusPath)result("org.freedesktop.portal.OpenURI","OpenURI","ssa{sv}","o",parent,uri,portalMap(options));}
        @Override public DBusPath OpenFile(String parent,org.freedesktop.dbus.FileDescriptor fd,Map<String,Variant<?>> options){return (DBusPath)result("org.freedesktop.portal.OpenURI","OpenFile","sha{sv}","o",parent,fd,portalMap(options));}
        @Override public DBusPath OpenDirectory(String parent,org.freedesktop.dbus.FileDescriptor fd,Map<String,Variant<?>> options){return (DBusPath)result("org.freedesktop.portal.OpenURI","OpenDirectory","sha{sv}","o",parent,fd,portalMap(options));}
    }
    private final class RequestObject extends BaseObject implements Request {
        RequestObject(){super(PortalBackend.DESKTOP+"/request",false,false);}
        @Override public void Close(){result("org.freedesktop.portal.Request","Close","","",new Object[0]);}
    }
    private final class SessionObject extends BaseObject implements Session {
        SessionObject(){super(PortalBackend.DESKTOP+"/session",false,true);}
        @Override public void Close(){result("org.freedesktop.portal.Session","Close","","",new Object[0]);}
    }
    private static int signatureEnd(String signature,int at){char c=signature.charAt(at++);if(c=='a')return signatureEnd(signature,at);if(c=='('||c=='{'){char close=c=='('?')':'}';while(signature.charAt(at)!=close)at=signatureEnd(signature,at);return at+1;}return at;}
    private static String signaturePart(String signature,int index){int at=0;while(index-->0)at=signatureEnd(signature,at);return signature.substring(at,signatureEnd(signature,at));}
}
