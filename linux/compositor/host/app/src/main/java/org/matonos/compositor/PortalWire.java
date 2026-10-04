package org.matonos.compositor;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Bounded D-Bus wire codec, independent of Android for host unit tests. */
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
    }
    private static int alignment(char c) {
        switch(c) {
            case 'y': case 'g': case 'v': return 1;
            case 'n': case 'q': return 2;
            case 'x': case 't': case 'd': case '(': case '{': return 8;
            default: return 4;
        }
    }
    private static int end(String s,int start) {
        if(start>=s.length())throw new IllegalArgumentException("Incomplete signature");
        char c=s.charAt(start++);
        if(c=='a')return end(s,start);
        if(c=='('||c=='{') {
            char close=c=='('?')':'}';
            while(start<s.length() && s.charAt(start)!=close)start=end(s,start);
            if(start==s.length())throw new IllegalArgumentException("Unclosed signature");
            return start+1;
        }
        if("ybnqiuxtdsoghv".indexOf(c)<0)throw new IllegalArgumentException("Invalid type");
        return start;
    }
    private static final class Reader {
        final ByteBuffer b;
        int depth;
        Reader(byte[] bytes) {
            b=ByteBuffer.wrap(bytes);
            if(bytes.length<16 || (bytes[0]!='l' && bytes[0]!='B'))throw new IllegalArgumentException("Invalid header");
            b.order(bytes[0]=='l'?ByteOrder.LITTLE_ENDIAN:ByteOrder.BIG_ENDIAN);
        }
        void align(int n) { b.position((b.position()+n-1)&-n); }
        Object value(String s) {
            if(++depth>32)throw new IllegalArgumentException("Too deeply nested");
            try {
                char c=s.charAt(0);align(alignment(c));
                switch(c) {
                    case 'y': return b.get()&255;
                    case 'n': return b.getShort();
                    case 'q': return b.getShort()&65535;
                    case 'b': {int v=b.getInt();if(v!=0&&v!=1)throw new IllegalArgumentException("Invalid boolean");return v!=0;}
                    case 'u': case 'i': case 'h': return b.getInt();
                    case 'x': case 't': return b.getLong();
                    case 'd': return b.getDouble();
                    case 's': case 'o': case 'g': {
                        int n=c=='g'?(b.get()&255):b.getInt();
                        if(n<0 || n>=b.remaining())throw new IllegalArgumentException("Invalid string length");
                        byte[] data=new byte[n];b.get(data);
                        if(b.get()!=0)throw new IllegalArgumentException("Missing string terminator");
                        return new String(data,StandardCharsets.UTF_8);
                    }
                    case 'v': {String sig=(String)value("g");if(end(sig,0)!=sig.length())throw new IllegalArgumentException("Variant signature");return new Variant(sig,value(sig));}
                    case 'a': {
                        int size=b.getInt();String element=s.substring(1);align(alignment(element.charAt(0)));
                        if(size<0||size>b.remaining())throw new IllegalArgumentException("Invalid array length");
                        int limit=b.position()+size;List<Object> values=new ArrayList<>();
                        while(b.position()<limit) {if(values.size()>=65536)throw new IllegalArgumentException("Array too large");values.add(value(element));}
                        if(b.position()!=limit)throw new IllegalArgumentException("Array boundary");
                        return values;
                    }
                    case '(': case '{': {
                        List<Object> values=new ArrayList<>();
                        for(int i=1;i<s.length()-1;) {int next=end(s,i);values.add(value(s.substring(i,next)));i=next;}
                        return values;
                    }
                    default: throw new IllegalArgumentException("Unsupported type");
                }
            } finally {depth--;}
        }
    }
    private static final class Writer {
        final ByteBuffer b=ByteBuffer.allocate(MAX_MESSAGE).order(ByteOrder.LITTLE_ENDIAN);
        void align(int n) {while((b.position()&(n-1))!=0)b.put((byte)0);}
        void value(String s,Object obj) {
            char c=s.charAt(0);align(alignment(c));
            switch(c) {
                case 'y': b.put(((Number)obj).byteValue());break;
                case 'n': case 'q': b.putShort(((Number)obj).shortValue());break;
                case 'b': b.putInt((Boolean)obj?1:0);break;
                case 'u': case 'i': case 'h': b.putInt(((Number)obj).intValue());break;
                case 'x': case 't': b.putLong(((Number)obj).longValue());break;
                case 'd': b.putDouble(((Number)obj).doubleValue());break;
                case 's': case 'o': case 'g': {
                    byte[] bytes=((String)obj).getBytes(StandardCharsets.UTF_8);
                    if(c=='g') {if(bytes.length>255)throw new IllegalArgumentException("Signature length");b.put((byte)bytes.length);}else b.putInt(bytes.length);
                    b.put(bytes).put((byte)0);break;
                }
                case 'v': {Variant v=(Variant)obj;value("g",v.signature);value(v.signature,v.value);break;}
                case 'a': {
                    int lengthAt=b.position();b.putInt(0);String element=s.substring(1);align(alignment(element.charAt(0)));int start=b.position();
                    for(Object v:(List<?>)obj)value(element,v);
                    b.putInt(lengthAt,b.position()-start);break;
                }
                case '(': case '{': {
                    List<?> values=(List<?>)obj;int index=0;
                    for(int i=1;i<s.length()-1;) {int next=end(s,i);value(s.substring(i,next),values.get(index++));i=next;}break;
                }
                default: throw new IllegalArgumentException("Unsupported output type");
            }
        }
        void field(int id,String sig,Object obj) {if(obj!=null)value("(yv)",java.util.Arrays.asList(id,new Variant(sig,obj)));}
    }
    static Message decode(byte[] bytes) {
        if(bytes.length>MAX_MESSAGE)throw new IllegalArgumentException("Message too large");
        Reader r=new Reader(bytes);ByteBuffer b=r.b;Message m=new Message();
        b.position(1);m.type=b.get()&255;m.flags=b.get()&255;
        if(b.get()!=1 || m.type<1 || m.type>4)throw new IllegalArgumentException("Protocol version/type");
        int bodyLength=b.getInt();m.serial=b.getInt();int headerLength=b.getInt();
        if(m.serial==0||headerLength<0||headerLength>bytes.length-16)throw new IllegalArgumentException("Invalid header length/serial");
        int headerEnd=16+headerLength;
        while(b.position()<headerEnd) {
            List<?> f=(List<?>)r.value("(yv)");int id=(Integer)f.get(0);Variant v=(Variant)f.get(1);
            switch(id) {
                case 1: m.path=(String)v.value;break;case 2:m.iface=(String)v.value;break;
                case 3:m.member=(String)v.value;break;case 4:m.error=(String)v.value;break;
                case 5:m.replySerial=(Integer)v.value;break;case 6:m.destination=(String)v.value;break;
                case 7:m.sender=(String)v.value;break;case 8:m.signature=(String)v.value;break;
                case 9:m.fdCount=(Integer)v.value;break;default:break;
            }
        }
        if(b.position()!=headerEnd)throw new IllegalArgumentException("Header boundary");
        r.align(8);
        if(bodyLength<0 || bodyLength!=b.remaining())throw new IllegalArgumentException("Body length");
        for(int i=0;i<m.signature.length();) {int next=end(m.signature,i);m.body.add(r.value(m.signature.substring(i,next)));i=next;}
        if(b.hasRemaining())throw new IllegalArgumentException("Trailing body data");
        return m;
    }
    static byte[] encode(Message m) {
        Writer w=new Writer();ByteBuffer b=w.b;
        b.put((byte)'l').put((byte)m.type).put((byte)m.flags).put((byte)1).putInt(0).putInt(m.serial).putInt(0);
        w.field(1,"o",m.path);w.field(2,"s",m.iface);w.field(3,"s",m.member);w.field(4,"s",m.error);
        if(m.replySerial!=0)w.field(5,"u",m.replySerial);
        w.field(6,"s",m.destination);w.field(7,"s",m.sender);
        if(!m.signature.isEmpty())w.field(8,"g",m.signature);
        if(m.fdCount!=0)w.field(9,"u",m.fdCount);
        b.putInt(12,b.position()-16);w.align(8);int bodyStart=b.position(),index=0;
        for(int i=0;i<m.signature.length();) {int next=end(m.signature,i);w.value(m.signature.substring(i,next),m.body.get(index++));i=next;}
        b.putInt(4,b.position()-bodyStart);return java.util.Arrays.copyOf(b.array(),b.position());
    }
    static Map<String,Variant> dictionary(Object array) {
        Map<String,Variant> result=new LinkedHashMap<>();
        for(Object entry:(List<?>)array) {List<?> e=(List<?>)entry;result.put((String)e.get(0),(Variant)e.get(1));}
        return result;
    }
    static List<Object> dictionary(Map<String,Variant> values) {
        List<Object> result=new ArrayList<>();values.forEach((k,v)->result.add(java.util.Arrays.asList(k,v)));return result;
    }
}
