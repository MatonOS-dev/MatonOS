package org.matonos.compositor;

import android.app.UiModeManager;
import android.content.Context;
import android.content.Intent;
import android.content.ClipData;
import android.content.res.Configuration;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.function.Consumer;
import static org.matonos.compositor.PortalWire.*;

/** One private backend channel per SessionBus, with no native Android logic. */
final class JavaPortal implements AutoCloseable,PortalBackend.Platform {
    interface Launcher {boolean launch(Intent intent) throws Exception;}
    private final Context context;
    private final Consumer<Boolean> changed;
    private final Launcher launcher;
    private final LocalSocket bound=new LocalSocket();
    private final LocalServerSocket server;
    private final File path;
    private final PortalBackend backend=new PortalBackend(this);
    private final byte[] secret=new byte[32];
    private final Set<Object> fileOwners=Collections.newSetFromMap(new IdentityHashMap<>());
    private volatile LocalSocket client;
    private volatile boolean stopped, held;
    private ParcelFileDescriptor[] descriptors;
    private String owner;
    JavaPortal(Context context,File directory,Consumer<Boolean> changed,Launcher launcher) throws Exception {
        this.context=context;this.changed=changed;this.launcher=launcher;
        new java.security.SecureRandom().nextBytes(secret);
        path=new File(directory,"portal-backend");if(path.exists()&&!path.delete())throw new IOException("Stale portal socket");
        bound.bind(new LocalSocketAddress(path.getAbsolutePath(),LocalSocketAddress.Namespace.FILESYSTEM));Os.chmod(path.getAbsolutePath(),0600);
        server=new LocalServerSocket(bound.getFileDescriptor());new Thread(this::serve,"java-portals").start();
    }
    String secret() {StringBuilder s=new StringBuilder();for(byte b:secret)s.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return s.toString();}
    boolean isHeld() {return held;}
    private static void readFully(InputStream in,byte[] data) throws IOException {
        int offset=0;while(offset<data.length){int n=in.read(data,offset,data.length-offset);if(n<=0)throw new EOFException();offset+=n;}
    }
    private void serve() {
        try {
            // Exactly one authenticated broker, no reconnect into stale request state.
            while(!stopped) {
                LocalSocket socket=server.accept();client=socket;socket.setSoTimeout(3000);
                byte[] token=new byte[64];
                try {
                    if(socket.getPeerCredentials().getUid()!=android.os.Process.myUid())throw new IOException("Wrong backend UID");
                    byte[] magic=new byte[4];readFully(socket.getInputStream(),magic);
                    if(!Arrays.equals(magic,new byte[]{'M','B','P','1'}))throw new IOException("Unsupported portal protocol");
                    readFully(socket.getInputStream(),token);
                    if(!java.security.MessageDigest.isEqual(token,secret().getBytes(StandardCharsets.US_ASCII)))throw new IOException("Wrong backend capability");
                }catch(Exception e){socket.close();client=null;continue;}
                socket.getOutputStream().write(new byte[]{'O','K','A','Y'});
                socket.setSoTimeout(0);run(socket);break;
            }
        }catch(Exception e){if(!stopped)Log.w("MatonPortal","Backend disconnected",e);}
        finally {
            try {backend.close();}catch(Exception e){Log.w("MatonPortal","Hold release failed",e);}
            synchronized(fileOwners){for(Object o:fileOwners)PortalFiles.release(context,o);fileOwners.clear();}
            try {if(client!=null)client.close();}catch(Exception ignored){}
            try {server.close();}catch(Exception ignored){}
            try {bound.close();}catch(Exception ignored){}
        }
    }
    private void run(LocalSocket socket) throws Exception {
        InputStream in=socket.getInputStream();OutputStream out=socket.getOutputStream();
        while(!stopped) {
            List<FileDescriptor> received=new ArrayList<>();
            try {
                byte[] header=new byte[8];int offset=0;
                // Read the header separately: rights belong to its first byte,
                // never consume bytes from the following frame before collecting them.
                while(offset<8) {int n=in.read(header,offset,8-offset);if(n<=0)throw new EOFException();offset+=n;collect(socket,received);}
                ByteBuffer h=ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);int size=h.getInt(),fds=h.getInt();
                if(size<16||size>MAX_MESSAGE||fds<0||fds>MAX_FDS||fds!=received.size())throw new IOException("Invalid portal frame");
                byte[] bytes=new byte[size];readFully(in,bytes);collect(socket,received);
                if(received.size()!=fds)throw new IOException("Unexpected portal descriptors");
                Message call=decode(bytes);if(call.fdCount!=fds)throw new IOException("D-Bus descriptor count mismatch");
                descriptors=new ParcelFileDescriptor[fds];
                for(int i=0;i<fds;i++)descriptors[i]=ParcelFileDescriptor.dup(received.get(i));
                if(call.type==4 && "org.matonos.PortalBackend".equals(call.iface)&&"ClientClosed".equals(call.member)&&"s".equals(call.signature)) {
                    String closed=(String)call.body.get(0);backend.disconnected(closed);
                    synchronized(fileOwners){Iterator<Object> it=fileOwners.iterator();while(it.hasNext()){Object o=it.next();if(o.toString().equals(closed)){PortalFiles.release(context,o);it.remove();}}}
                    continue;
                }
                if(call.type!=1)throw new IOException("Expected portal call");
                owner=call.sender;
                for(Message reply:backend.dispatch(call)) {
                    if(reply.type!=4 && (call.flags&1)!=0)continue;
                    byte[] encoded=encode(reply);out.write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(encoded.length).putInt(0).array());out.write(encoded);
                }
            } finally {
                if(descriptors!=null)for(ParcelFileDescriptor fd:descriptors)if(fd!=null)fd.close();descriptors=null;
                for(FileDescriptor fd:received)try{Os.close(fd);}catch(Exception ignored){}
            }
        }
    }
    private static void collect(LocalSocket socket,List<FileDescriptor> received) throws IOException {
        FileDescriptor[] rights=socket.getAncillaryFileDescriptors();if(rights!=null)Collections.addAll(received,rights);
        if(received.size()>MAX_FDS)throw new IOException("Too many received FDs");
    }
    @Override public Map<String,Map<String,Variant>> settings() {
        Map<String,Variant> appearance=new LinkedHashMap<>();
        int mode=context.getResources().getConfiguration().uiMode&Configuration.UI_MODE_NIGHT_MASK;
        UiModeManager ui=context.getSystemService(UiModeManager.class);
        if(ui!=null) {int night=ui.getNightMode();if(night==UiModeManager.MODE_NIGHT_YES)mode=Configuration.UI_MODE_NIGHT_YES;else if(night==UiModeManager.MODE_NIGHT_NO)mode=Configuration.UI_MODE_NIGHT_NO;}
        appearance.put("color-scheme",new Variant("u",mode==Configuration.UI_MODE_NIGHT_YES?1:mode==Configuration.UI_MODE_NIGHT_NO?2:0));
        appearance.put("contrast",new Variant("u",android.os.Build.VERSION.SDK_INT>=34&&ui!=null&&ui.getContrast()>0?1:0));
        try {
            if(android.os.Build.VERSION.SDK_INT<31)throw new android.content.res.Resources.NotFoundException();
            int color=context.getColor(android.R.color.system_accent1_500);
            appearance.put("accent-color",new Variant("(ddd)",Arrays.asList(((color>>16)&255)/255.0,((color>>8)&255)/255.0,(color&255)/255.0)));
        }catch(android.content.res.Resources.NotFoundException ignored){}
        Map<String,Map<String,Variant>> result=new LinkedHashMap<>();result.put("org.freedesktop.appearance",appearance);
        result.put("org.gnome.desktop.wm.preferences",Collections.singletonMap("button-layout",new Variant("s",":")));return result;
    }
    @Override public synchronized void hold(boolean active) {
        active=active&&!stopped;
        if(active==held)return;
        // The verified stub acquires/releases under its own UID via changed.
        held=active;
        try{changed.accept(active);}catch(RuntimeException e){Log.w("MatonPortal","Inhibit listener failed",e);}
    }
    @Override public boolean open(String method,Object target,Map<String,Variant> options) throws Exception {
        if(stopped)return false;
        Variant ask=options.get("ask");
        if(ask!=null && !"b".equals(ask.signature))return false;
        Uri uri;boolean writable=false;Object fileOwner=null;
        if("OpenURI".equals(method)) {
            uri=Uri.parse((String)target);String scheme=uri.getScheme();
            // Files must arrive by capability FD; never open host paths or
            // forward arbitrary Android provider URIs with host authority.
            if(scheme==null||scheme.equalsIgnoreCase("file")||scheme.equalsIgnoreCase("content")||scheme.equalsIgnoreCase("intent"))return false;
        }else {
            int index=(Integer)target;if(index<0||descriptors==null||index>=descriptors.length)return false;
            android.system.StructStat stat=Os.fstat(descriptors[index].getFileDescriptor());boolean directory="OpenDirectory".equals(method);
            if(directory?!OsConstants.S_ISDIR(stat.st_mode):!(OsConstants.S_ISREG(stat.st_mode)||OsConstants.S_ISDIR(stat.st_mode)))return false;
            directory=OsConstants.S_ISDIR(stat.st_mode);
            Variant write=options.get("writable");if(write!=null){if(!"b".equals(write.signature))return false;writable=(Boolean)write.value;}
            int flags=Os.fcntlInt(descriptors[index].getFileDescriptor(),OsConstants.F_GETFL,0);
            if((flags&OsConstants.O_ACCMODE)==OsConstants.O_WRONLY || (writable&&(flags&OsConstants.O_ACCMODE)==OsConstants.O_RDONLY))return false;
            fileOwner=new String(owner);uri=PortalFiles.publish(descriptors[index],directory,writable,fileOwner);
            synchronized(fileOwners){if(stopped){PortalFiles.revoke(context,uri);return false;}fileOwners.add(fileOwner);}
        }
        Intent intent=new Intent(Intent.ACTION_VIEW).addCategory(Intent.CATEGORY_DEFAULT);
        if(fileOwner!=null) {
            intent.setDataAndType(uri,context.getContentResolver().getType(uri));
            intent.setClipData(ClipData.newRawUri("Linux document",uri));intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            if(writable)intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        }else intent.setData(uri);
        if(ask!=null && (Boolean)ask.value)intent=Intent.createChooser(intent,"Open with");
        try {
            boolean ok=launcher.launch(intent);
            if(!ok&&fileOwner!=null){PortalFiles.revoke(context,uri);synchronized(fileOwners){fileOwners.remove(fileOwner);}}
            return ok;
        }catch(Exception e){if(fileOwner!=null){PortalFiles.revoke(context,uri);synchronized(fileOwners){fileOwners.remove(fileOwner);}}return false;}
    }
    @Override public void close() {
        stopped=true;
        try{server.close();}catch(Exception ignored){}
        try{bound.close();}catch(Exception ignored){}
        try{if(client!=null)client.close();}catch(Exception ignored){}
        hold(false);synchronized(fileOwners){for(Object o:fileOwners)PortalFiles.release(context,o);fileOwners.clear();}path.delete();
    }
}
