package org.matonos.compositor;

import org.freedesktop.dbus.DBusPath;
import org.freedesktop.dbus.Struct;
import org.freedesktop.dbus.annotations.Position;
import org.freedesktop.dbus.exceptions.DBusException;
import org.freedesktop.dbus.messages.DBusSignal;
import org.freedesktop.dbus.messages.Error;
import org.freedesktop.dbus.messages.MessageFactory;
import org.freedesktop.dbus.messages.MethodCall;
import org.freedesktop.dbus.types.UInt16;
import org.freedesktop.dbus.types.UInt32;
import org.freedesktop.dbus.types.UInt64;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Compatibility view over dbus-java messages; D-Bus value framing is owned by dbus-java. */
final class PortalWire {
    static final int MAX_MESSAGE = 1024 * 1024, MAX_FDS = 16;

    static final class Variant {
        final String signature;
        final Object value;
        Variant(String signature, Object value) { this.signature=signature; this.value=value; }
    }

    static final class Message {
        int type, flags, serial, replySerial, fdCount;
        String path, iface, member, error, destination, sender, signature="";
        List<Object> body = new ArrayList<>();
        org.freedesktop.dbus.messages.Message dbusMessage;
        org.freedesktop.dbus.messages.Message replyTo;
    }

    public static final class AccentColor extends Struct {
        @Position(0) public double red;
        @Position(1) public double green;
        @Position(2) public double blue;
        public AccentColor(double red,double green,double blue){this.red=red;this.green=green;this.blue=blue;}
    }

    private static final MessageFactory FACTORY = new MessageFactory((byte) 'l');

    static Message decode(byte[] bytes) {
        return decode(bytes,null);
    }

    static Message decode(byte[] bytes,List<org.freedesktop.dbus.FileDescriptor> descriptors) {
        if (bytes.length < 16 || bytes.length > MAX_MESSAGE) throw new IllegalArgumentException("Invalid message size");
        ByteOrder order = bytes[0] == 'l' ? ByteOrder.LITTLE_ENDIAN : bytes[0] == 'B' ? ByteOrder.BIG_ENDIAN : null;
        if (order == null || bytes[3] != 1) throw new IllegalArgumentException("Invalid D-Bus header");
        ByteBuffer b = ByteBuffer.wrap(bytes).order(order);
        int type = bytes[1] & 255, bodySize = b.getInt(4), serial = b.getInt(8), headerSize = b.getInt(12);
        if (type < 1 || type > 4 || serial == 0 || headerSize < 0 || bodySize < 0) throw new IllegalArgumentException("Invalid D-Bus header fields");
        int paddedHeader = (headerSize + 7) & ~7;
        if (paddedHeader < headerSize || 16L + paddedHeader + bodySize != bytes.length) throw new IllegalArgumentException("Invalid D-Bus message length");
        byte[] fixed = Arrays.copyOfRange(bytes, 0, 12);
        byte[] header = new byte[paddedHeader + 8];
        System.arraycopy(bytes, 12, header, 0, 4);
        System.arraycopy(bytes, 16, header, 8, headerSize);
        byte[] body = Arrays.copyOfRange(bytes, 16 + paddedHeader, bytes.length);
        int declaredFds=fdCount(bytes,headerSize,order);
        final org.freedesktop.dbus.messages.Message dm;
        try { dm = MessageFactory.createMessage((byte) type, fixed, header, body, descriptors); }
        catch (Exception e) { throw new IllegalArgumentException("Malformed D-Bus message", e); }
        Message decoded=fromDbus(dm);if(decoded.fdCount!=declaredFds)throw new IllegalArgumentException("D-Bus descriptor count mismatch");return decoded;
    }

    static Message fromDbus(org.freedesktop.dbus.messages.Message dm) {
        Message m = new Message(); m.dbusMessage=dm; m.type=dm.getType(); m.flags=dm.getFlags(); m.serial=(int)dm.getSerial();
        m.replySerial=(int)dm.getReplySerial(); m.path=dm.getPath(); m.iface=dm.getInterface(); m.member=dm.getName();
        m.error=dm instanceof Error ? dm.getName() : null; m.destination=dm.getDestination(); m.sender=dm.getSource();
        m.signature=dm.getSig()==null?"":dm.getSig(); m.fdCount=dm.getFiledescriptors().size();
        try {
            Object[] args=dm.getParameters();
            for(int i=0;i<args.length;i++)m.body.add(fromDbus(signaturePart(m.signature,i),args[i]));
        } catch (DBusException | RuntimeException e) { throw new IllegalArgumentException("Malformed D-Bus body", e); }
        return m;
    }

    static byte[] encode(Message m) {
        org.freedesktop.dbus.messages.Message dm=toDbusMessage(m);
        byte[][] wire=dm.getWireData(); int n=0; for(byte[] part:wire){if(part==null)break;n=Math.addExact(n,part.length);}
        if(n>MAX_MESSAGE)throw new IllegalArgumentException("D-Bus message too large");
        byte[] out=new byte[n];int at=0;for(byte[] part:wire){if(part==null)break;System.arraycopy(part,0,out,at,part.length);at+=part.length;}
        return out;
    }

    static org.freedesktop.dbus.messages.Message toDbusMessage(Message m) {
        try {
            String sig=m.signature==null||m.signature.isEmpty()?null:m.signature;
            Object[] args=toDbusArgs(m.signature,m.body);
            org.freedesktop.dbus.messages.Message dm;
            if(m.type==1) {
                dm=FACTORY.createMethodCall(m.sender,m.destination,m.path,m.iface,m.member,(byte)m.flags,sig,args);
            } else if(m.type==2) {
                if(!(m.replyTo instanceof MethodCall))throw new IllegalArgumentException("Reply lacks dbus-java call context");
                dm=FACTORY.createMethodReturn(m.sender,(MethodCall)m.replyTo,sig,args);
            } else if(m.type==3) {
                dm=FACTORY.createError(m.sender,m.destination,m.error,m.replySerial,sig,args);
            } else if(m.type==4) {
                dm=FACTORY.createSignal(m.sender,m.path,m.iface,m.member,sig,args);
            } else throw new IllegalArgumentException("Unsupported D-Bus message type");
            m.dbusMessage=dm;return dm;
        } catch (DBusException e) { throw new IllegalArgumentException("dbus-java could not encode message", e); }
    }

    private static Object[] toDbusArgs(String sig,List<Object> values) {
        if(sig==null||sig.isEmpty())return new Object[0];
        List<Object> out=new ArrayList<>();int at=0;
        for(int p=0;p<sig.length();) {int end=signatureEnd(sig,p);out.add(toDbus(sig.substring(p,end),values.get(at++)));p=end;}
        if(at!=values.size())throw new IllegalArgumentException("Signature/body arity mismatch");
        return out.toArray();
    }

    private static Object toDbus(String sig,Object value) {
        char c=sig.charAt(0);
        if(c=='v') { Variant v=(Variant)value; return new org.freedesktop.dbus.types.Variant<>(toDbus(v.signature,v.value),v.signature); }
        if(c=='u')return new UInt32(((Number)value).longValue());
        if(c=='q')return new UInt16(((Number)value).intValue());
        if(c=='t')return new UInt64(((Number)value).longValue());
        if(c=='o')return new DBusPath((String)value);
        if(c=='h'&&value instanceof Number)return new org.freedesktop.dbus.FileDescriptor(((Number)value).intValue());
        if(c=='a') {
            String element=sig.substring(1);
            if(element.startsWith("{")) {Map<Object,Object> map=new LinkedHashMap<>();for(Object entry:(List<?>)value){List<?> pair=(List<?>)entry;map.put(toDbus(element.substring(1,2),pair.get(0)),toDbus(element.substring(2,element.length()-1),pair.get(1)));}return map;}
            List<Object> out=new ArrayList<>();for(Object item:(List<?>)value)out.add(toDbus(element,item));return out;
        }
        if("(ddd)".equals(sig)) {List<?> rgb=(List<?>)value;return new AccentColor(((Number)rgb.get(0)).doubleValue(),((Number)rgb.get(1)).doubleValue(),((Number)rgb.get(2)).doubleValue());}
        return value;
    }

    private static Object fromDbus(String sig,Object value) {
        if(sig.isEmpty())return value;
        char c=sig.charAt(0);
        if(c=='v') {org.freedesktop.dbus.types.Variant<?> v=(org.freedesktop.dbus.types.Variant<?>)value;return new Variant(v.getSig(),fromDbus(v.getSig(),v.getValue()));}
        if(value instanceof UInt32)return ((UInt32)value).intValue();
        if(value instanceof UInt16)return ((UInt16)value).intValue();
        if(value instanceof UInt64)return ((UInt64)value).longValue();
        if(c=='h'&&value instanceof org.freedesktop.dbus.FileDescriptor)return ((org.freedesktop.dbus.FileDescriptor)value).getIntFileDescriptor();
        if(c=='o'&&value instanceof DBusPath)return ((DBusPath)value).getPath();
        if("(ddd)".equals(sig)&&value instanceof AccentColor){AccentColor rgb=(AccentColor)value;return Arrays.asList(rgb.red,rgb.green,rgb.blue);}
        if(c=='a') {
            String element=sig.substring(1);
            if(element.startsWith("{")) {List<Object> out=new ArrayList<>();for(Map.Entry<?,?> e:((Map<?,?>)value).entrySet())out.add(Arrays.asList(fromDbus(element.substring(1,2),e.getKey()),fromDbus(element.substring(2,element.length()-1),e.getValue())));return out;}
            List<Object> out=new ArrayList<>();for(Object item:(List<?>)value)out.add(fromDbus(element,item));return out;
        }
        return value;
    }

    private static String signaturePart(String signature,int index) {
        int p=0;while(index-->0)p=signatureEnd(signature,p);int e=signatureEnd(signature,p);return signature.substring(p,e);
    }

    private static int signatureEnd(String s,int p) {
        if(p>=s.length())throw new IllegalArgumentException("Incomplete D-Bus signature");char c=s.charAt(p++);
        if(c=='a')return signatureEnd(s,p);
        if(c=='('||c=='{'){char close=c=='('?')':'}';while(p<s.length()&&s.charAt(p)!=close)p=signatureEnd(s,p);if(p>=s.length())throw new IllegalArgumentException("Unclosed D-Bus signature");return p+1;}
        return p;
    }

    /** Reads only the standard UNIX_FDS header field; dbus-java parses all message fields and values. */
    static int declaredFdCount(byte[] bytes) {
        if(bytes.length<16)throw new IllegalArgumentException("Truncated D-Bus message");
        ByteOrder order=bytes[0]=='l'?ByteOrder.LITTLE_ENDIAN:bytes[0]=='B'?ByteOrder.BIG_ENDIAN:null;
        if(order==null)throw new IllegalArgumentException("Invalid D-Bus endianness");
        return fdCount(bytes,ByteBuffer.wrap(bytes).order(order).getInt(12),order);
    }

    private static int fdCount(byte[] bytes,int headerSize,ByteOrder order) {
        ByteBuffer b=ByteBuffer.wrap(bytes).order(order);b.position(16);int end=16+headerSize,count=0;boolean seen=false;
        while(b.position()<end) {
            int aligned=(b.position()+7)&~7;if(aligned>end)throw new IllegalArgumentException("Malformed header padding");b.position(aligned);
            if(b.position()==end)break;
            int id=b.get()&255;if(!b.hasRemaining())throw new IllegalArgumentException("Truncated header field");
            int sigLength=b.get()&255;if(sigLength==0||sigLength>b.remaining()-1)throw new IllegalArgumentException("Malformed header variant");
            byte[] sigBytes=new byte[sigLength];b.get(sigBytes);if(b.get()!=0)throw new IllegalArgumentException("Malformed header signature");
            String sig=new String(sigBytes,java.nio.charset.StandardCharsets.US_ASCII);if(sig.length()!=1)throw new IllegalArgumentException("Invalid header field type");
            char type=sig.charAt(0);int a=type=='y'||type=='g'?1:type=='n'||type=='q'?2:type=='x'||type=='t'||type=='d'?8:4;
            b.position((b.position()+a-1)&-a);
            if(id==9) {if(seen||type!='u'||b.position()+4>end)throw new IllegalArgumentException("Invalid UNIX_FDS header");count=b.getInt();if(count<0)throw new IllegalArgumentException("Invalid UNIX_FDS count");seen=true;}
            else if(type=='s'||type=='o') {if(b.position()+4>end)throw new IllegalArgumentException("Truncated header string");int n=b.getInt();if(n<0||b.position()+(long)n+1>end)throw new IllegalArgumentException("Invalid header string length");b.position(b.position()+n+1);}
            else if(type=='g') {if(b.position()>=end)throw new IllegalArgumentException("Truncated header signature");int n=b.get()&255;if(b.position()+(long)n+1>end)throw new IllegalArgumentException("Invalid header signature length");b.position(b.position()+n+1);}
            else if(type=='u'||type=='i') {if(b.position()+4>end)throw new IllegalArgumentException("Truncated header integer");if(id!=9)b.getInt();}
            else throw new IllegalArgumentException("Unsupported header field type");
        }
        if(b.position()!=end)throw new IllegalArgumentException("Header boundary mismatch");return count;
    }

    static Map<String,Variant> dictionary(Object array) {
        Map<String,Variant> result=new LinkedHashMap<>();
        for(Object entry:(List<?>)array){List<?> e=(List<?>)entry;result.put((String)e.get(0),(Variant)e.get(1));}
        return result;
    }
    static List<Object> dictionary(Map<String,Variant> values) {
        List<Object> result=new ArrayList<>();values.forEach((k,v)->result.add(Arrays.asList(k,v)));return result;
    }
}
