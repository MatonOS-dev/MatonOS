package org.matonos.compositor;

import android.net.LocalSocket;
import android.system.Os;
import org.freedesktop.dbus.connections.BusAddress;
import org.freedesktop.dbus.connections.config.TransportConfig;
import org.freedesktop.dbus.connections.transports.AbstractTransport;
import org.freedesktop.dbus.exceptions.TransportConfigurationException;
import org.freedesktop.dbus.messages.Message;
import org.freedesktop.dbus.messages.MessageFactory;
import org.freedesktop.dbus.spi.message.AbstractInputStreamMessageReader;
import org.freedesktop.dbus.spi.message.IMessageReader;
import org.freedesktop.dbus.spi.message.IMessageWriter;
import org.freedesktop.dbus.spi.message.ISocketProvider;
import org.freedesktop.dbus.spi.transport.ITransportProvider;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.EOFException;
import java.io.FileDescriptor;
import java.io.IOException;
import java.net.Socket;
import java.net.SocketAddress;
import java.net.SocketOption;
import java.nio.ByteBuffer;
import java.nio.channels.NetworkChannel;
import java.nio.channels.SocketChannel;
import java.nio.channels.spi.SelectorProvider;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/** dbus-java TransportProvider for an already authenticated Android LocalSocket. */
public final class MatonLocalTransportProvider implements ITransportProvider {
    private static final Map<String, LocalSocket> SOCKETS = new ConcurrentHashMap<>();
    static final PortalSocketProvider SOCKET_PROVIDER = new PortalSocketProvider();

    static String handoff(LocalSocket socket) {
        String token=java.util.UUID.randomUUID().toString().replace("-","");SOCKETS.put(token,socket);return token;
    }
    static FileDescriptor descriptorFor(int token){return PortalSocketChannel.fdFor(token).orElse(null);}
    static void releaseDescriptor(int token){FileDescriptor fd=PortalSocketChannel.release(token);if(fd!=null)try{Os.close(fd);}catch(Exception ignored){}}
    @Override public String getTransportName(){return "MatonOS Android LocalSocket";}
    @Override public String getSupportedBusType(){return "MATON";}
    @Override public String createDynamicSessionAddress(boolean listening){return null;}
    @Override public AbstractTransport createTransport(BusAddress address,TransportConfig config) throws TransportConfigurationException {
        if(!"maton".equalsIgnoreCase(address.getType()))return null;
        LocalSocket socket=SOCKETS.remove(address.getParameterValue("token"));
        if(socket==null)throw new TransportConfigurationException("No handed-off LocalSocket for transport address");
        return new MatonLocalTransport(address,config,socket);
    }

    static final class MatonLocalTransport extends AbstractTransport {
        private final LocalSocket socket;
        private PortalSocketChannel channel;
        MatonLocalTransport(BusAddress address,TransportConfig config,LocalSocket socket){super(address,config);this.socket=socket;}
        @Override protected boolean hasFileDescriptorSupport(){return true;}
        @Override protected SocketChannel connectImpl(){channel=new PortalSocketChannel(socket);return channel;}
        @Override protected SocketChannel acceptImpl() throws IOException {throw new IOException("MATON transport does not listen");}
        @Override protected void bindImpl() throws IOException {throw new IOException("MATON transport does not bind");}
        @Override protected void closeTransport() throws IOException {socket.close();}
        @Override protected boolean isBound(){return false;}
        PortalSocketChannel channel(){return channel;}
    }

    public static final class PortalSocketProvider implements ISocketProvider {
        public PortalSocketProvider(){}
        private boolean enabled;
        @Override public IMessageReader createReader(SocketChannel channel){return channel instanceof PortalSocketChannel?new Reader(channel,this):null;}
        @Override public IMessageWriter createWriter(SocketChannel channel){return channel instanceof PortalSocketChannel?new Writer((PortalSocketChannel)channel,this):null;}
        @Override public void setFileDescriptorSupport(boolean support){enabled=support;}
        @Override public boolean isFileDescriptorPassingSupported(){return enabled;}
        @Override public java.util.Optional<Integer> getFileDescriptorValue(FileDescriptor fd){return PortalSocketChannel.tokenFor(fd);}
        @Override public java.util.Optional<FileDescriptor> createFileDescriptor(int token){return PortalSocketChannel.fdFor(token);}
    }

    private static final class Reader extends AbstractInputStreamMessageReader {
        private final PortalSocketProvider provider;
        Reader(SocketChannel channel,PortalSocketProvider provider){super(channel,provider);this.provider=provider;}
        @Override protected List<org.freedesktop.dbus.FileDescriptor> readFileDescriptors(SocketChannel input){
            if(!provider.isFileDescriptorPassingSupported())return null;
            PortalSocketChannel channel=(PortalSocketChannel)input;
            List<FileDescriptor> actual=channel.takeReceivedDescriptors();
            if(PortalWire.declaredFdCount(channel.frameBytes())!=actual.size())throw new IllegalStateException("D-Bus UNIX_FDS header does not match frame");
            if(actual.isEmpty())return Collections.emptyList();
            List<org.freedesktop.dbus.FileDescriptor> values=new ArrayList<>();
            for(FileDescriptor fd:actual)values.add(new org.freedesktop.dbus.FileDescriptor(PortalSocketChannel.tokenFor(fd).orElseThrow()));
            return values;
        }
    }

    private static final class Writer implements IMessageWriter {
        private final PortalSocketChannel channel; private final PortalSocketProvider provider; private boolean closed;
        Writer(PortalSocketChannel channel,PortalSocketProvider provider){this.channel=channel;this.provider=provider;}
        @Override public synchronized void writeMessage(Message message)throws IOException {
            if(closed)throw new IOException("Writer closed");byte[][] parts=message.getWireData();ByteArrayOutputStream blob=new ByteArrayOutputStream();
            for(byte[] part:parts){if(part==null)break;blob.write(part);}byte[] bytes=blob.toByteArray();
            if(bytes.length<16||bytes.length>PortalWire.MAX_MESSAGE)throw new IOException("Invalid D-Bus output size");
            List<org.freedesktop.dbus.FileDescriptor> sent=message.getFiledescriptors();
            FileDescriptor[] rights=new FileDescriptor[sent.size()];for(int i=0;i<sent.size();i++){
                FileDescriptor fd=channel.fdForIndex(sent.get(i).getIntFileDescriptor());
                if(fd==null)throw new IOException("Unknown LocalSocket descriptor token");rights[i]=fd;
            }
            if(rights.length>PortalWire.MAX_FDS)throw new IOException("Too many descriptors");
            if(rights.length>0)channel.localSocket().setFileDescriptorsForSend(rights);
            ByteBuffer frame=ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(bytes.length).putInt(rights.length);
            channel.rawWrite(frame.array());channel.rawWrite(bytes);
        }
        @Override public boolean isClosed(){return closed;}
        @Override public void close()throws IOException{closed=true;if(channel.isOpen())channel.close();}
    }

    static final class PortalSocketChannel extends SocketChannel {
        private static final Object FD_LOCK=new Object();
        private static final Map<FileDescriptor,Integer> TOKENS=new IdentityHashMap<>();
        private static final Map<Integer,FileDescriptor> FDS=new ConcurrentHashMap<>();
        private static int nextToken=1;
        private final LocalSocket local;
        private final java.io.InputStream in;private final java.io.OutputStream out;
        private final ArrayDeque<Byte> auth=new ArrayDeque<>();private final ByteArrayOutputStream authCommand=new ByteArrayOutputStream();
        private final ArrayList<FileDescriptor> received=new ArrayList<>();
        private final ByteArrayOutputStream frameBytes=new ByteArrayOutputStream();
        private List<FileDescriptor> currentRights=Collections.emptyList();
        private int frameRemaining=-1,frameFds;private boolean dataMode;
        PortalSocketChannel(LocalSocket socket){super(SelectorProvider.provider());local=socket;try{in=socket.getInputStream();out=socket.getOutputStream();}catch(IOException e){throw new IllegalStateException(e);}}
        LocalSocket localSocket(){return local;}
        static java.util.Optional<Integer> tokenFor(FileDescriptor fd){synchronized(FD_LOCK){Integer token=TOKENS.get(fd);if(token==null){token=nextToken++;TOKENS.put(fd,token);FDS.put(token,fd);}return java.util.Optional.of(token);}}
        static java.util.Optional<FileDescriptor> fdFor(int token){return java.util.Optional.ofNullable(FDS.get(token));}
        static FileDescriptor release(int token){synchronized(FD_LOCK){FileDescriptor fd=FDS.remove(token);if(fd!=null)TOKENS.remove(fd);return fd;}}
        List<FileDescriptor> takeReceivedDescriptors(){ArrayList<FileDescriptor> result=new ArrayList<>(received);received.clear();if(result.size()!=frameFds)throw new IllegalStateException("SCM_RIGHTS count does not match frame");currentRights=result;frameRemaining=-1;return result;}
        List<FileDescriptor> currentRights(){return new ArrayList<>(currentRights);}
        byte[] frameBytes(){return frameBytes.toByteArray();}
        FileDescriptor fdForIndex(int index){return index>=0&&index<currentRights.size()?currentRights.get(index):null;}
        private void authReply(String line){
            // MBP1 plus SO_PEERCRED already authenticates the actual channel.
            // AbstractTransport requires a SASL exchange, so answer that state
            // machine locally; these lines are never sent to the broker.
            String response=null;
            if(line.equals("AUTH"))response="REJECTED EXTERNAL\r\n";
            else if(line.startsWith("AUTH EXTERNAL "))response="OK 0123456789abcdef0123456789abcdef\r\n";
            else if(line.equals("NEGOTIATE_UNIX_FD"))response="AGREE_UNIX_FD\r\n";
            else if(line.equals("BEGIN")){dataMode=true;return;}
            else if(line.isEmpty())return;
            else response="ERROR Unsupported\r\n";
            for(byte b:response.getBytes(java.nio.charset.StandardCharsets.US_ASCII))auth.add(b);
        }
        @Override public int read(ByteBuffer dst)throws IOException {
            if(!dataMode){if(auth.isEmpty())return 0;int n=0;while(dst.hasRemaining()&&!auth.isEmpty()){dst.put(auth.remove());n++;}return n;}
            if(frameRemaining<0){byte[] header=new byte[8];rawReadFully(header);collectRights();int[] frame;try{frame=PortalWire.validateFrameHeader(header);}catch(IllegalArgumentException e){throw new IOException("Malformed D-Bus frame",e);}frameRemaining=frame[0];frameFds=frame[1];frameBytes.reset();}
            if(frameRemaining==0)return -1;int n=Math.min(dst.remaining(),frameRemaining);byte[] buf=new byte[n];int got=in.read(buf);if(got<0)throw new EOFException();dst.put(buf,0,got);frameBytes.write(buf,0,got);frameRemaining-=got;collectRights();if(got==0)return 0;return got;
        }
        private void collectRights()throws IOException{FileDescriptor[] fds=local.getAncillaryFileDescriptors();if(fds!=null){Collections.addAll(received,fds);if(received.size()>PortalWire.MAX_FDS)throw new IOException("Too many SCM_RIGHTS descriptors");}}
        @Override public long read(ByteBuffer[] dst,int offset,int length)throws IOException{long n=0;for(int i=offset;i<offset+length;i++){int r=read(dst[i]);if(r<=0)return n==0?r:n;n+=r;if(dst[i].hasRemaining())break;}return n;}
        @Override public int write(ByteBuffer src)throws IOException {
            if(dataMode){int n=src.remaining();byte[] b=new byte[n];src.get(b);out.write(b);return n;}
            int n=src.remaining();while(src.hasRemaining()){byte b=src.get();if(b==0)continue;if(b=='\n'){String line=new String(authCommand.toByteArray(),java.nio.charset.StandardCharsets.US_ASCII).replace("\r","");authCommand.reset();authReply(line);}else authCommand.write(b);}return n;
        }
        @Override public long write(ByteBuffer[] src,int offset,int length)throws IOException{long n=0;for(int i=offset;i<offset+length;i++)n+=write(src[i]);return n;}
        void rawReadFully(byte[] b)throws IOException{int at=0;while(at<b.length){int n=in.read(b,at,b.length-at);if(n<0)throw new EOFException();at+=n;}}
        void rawWrite(byte[] b)throws IOException{out.write(b);out.flush();}
        @Override protected void implCloseSelectableChannel()throws IOException{local.close();}
        @Override protected void implConfigureBlocking(boolean block){}
        @Override public SocketChannel bind(SocketAddress local)throws IOException{throw new UnsupportedOperationException();}
        @Override public <T> SocketChannel setOption(SocketOption<T> name,T value){throw new UnsupportedOperationException();}
        @Override public SocketChannel shutdownInput()throws IOException{throw new UnsupportedOperationException();}
        @Override public SocketChannel shutdownOutput()throws IOException{throw new UnsupportedOperationException();}
        @Override public Socket socket(){return null;}
        @Override public boolean isConnected(){return true;}
        @Override public boolean isConnectionPending(){return false;}
        @Override public boolean connect(SocketAddress remote){throw new UnsupportedOperationException();}
        @Override public boolean finishConnect(){return true;}
        @Override public SocketAddress getRemoteAddress(){return null;}
        @Override public SocketAddress getLocalAddress(){return null;}
        @Override public <T>T getOption(SocketOption<T> name){return null;}
        @Override public Set<SocketOption<?>> supportedOptions(){return Collections.emptySet();}
    }
}
