package org.matonos.store.fdroid;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;

/** Reads the package and split attributes of a base or standalone split APK.
 * PackageManager's archive parser requires a base and cannot name lone splits.
 */
final class ApkManifest {
  static String sessionName(File apk, String expectedPackage) throws IOException {
    byte[] bytes;
    try (ZipFile zip = new ZipFile(apk)) {
      ZipEntry entry = zip.getEntry("AndroidManifest.xml");
      if (entry == null) throw new IOException("APK has no AndroidManifest.xml: " + apk);
      try (InputStream in = zip.getInputStream(entry)) { bytes = in.readAllBytes(); }
    }
    try {
      ByteBuffer b = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
      if (u16(b, 0) != 3 || b.getInt(4) != bytes.length) throw new IOException("Invalid binary APK manifest");
      List<String> strings = null;
      for (int offset = u16(b, 2); offset < bytes.length;) {
        int type = u16(b, offset), header = u16(b, offset + 2), size = b.getInt(offset + 4);
        if (header < 8 || size < header || size > bytes.length - offset) throw new IOException("Invalid manifest chunk");
        if (type == 1) strings = stringPool(b, offset, header, size);
        if (type == 0x102 && strings != null) {
          int ext = offset + header;
          if (!"manifest".equals(strings.get(b.getInt(ext + 4)))) { offset += size; continue; }
          int start = u16(b, ext + 8), attrSize = u16(b, ext + 10), count = u16(b, ext + 12);
          if (attrSize < 20) throw new IOException("Invalid manifest attributes");
          String pkg = null, split = null;
          for (int i=0;i<count;i++) {
            int attr = ext + start + i * attrSize;
            if (attr < offset || attr + 20 > offset + size) throw new IOException("Manifest attribute out of bounds");
            // package/split are unnamespaced root attributes.
            if (b.getInt(attr) != -1) continue;
            String name = strings.get(b.getInt(attr + 4));
            int raw = b.getInt(attr + 8), dataType = b.get(attr + 15) & 255;
            String value = raw >= 0 ? strings.get(raw) : dataType == 3 ? strings.get(b.getInt(attr + 16)) : null;
            if ("package".equals(name)) pkg = value;
            if ("split".equals(name)) split = value;
          }
          if (!expectedPackage.equals(pkg)) throw new IOException("APK package " + pkg + " differs from " + expectedPackage);
          if (split == null || split.isEmpty()) return "base.apk";
          if (!split.matches("[A-Za-z0-9._-]+")) throw new IOException("Invalid split name " + split);
          return "split_" + split + ".apk";
        }
        offset += size;
      }
      throw new IOException("APK manifest root missing");
    } catch (IndexOutOfBoundsException | IllegalArgumentException error) { throw new IOException("Malformed APK manifest " + apk, error); }
  }
  private static int u16(ByteBuffer b, int p) { return b.getShort(p) & 65535; }
  private static int length(ByteBuffer b, int[] p, boolean utf8) {
    if (utf8) { int n=b.get(p[0]++) & 255; return (n & 128)==0 ? n : ((n & 127)<<8) | (b.get(p[0]++) & 255); }
    int n=u16(b,p[0]); p[0]+=2; if ((n & 32768)==0) return n;
    int low=u16(b,p[0]); p[0]+=2; return ((n & 32767)<<16) | low;
  }
  private static List<String> stringPool(ByteBuffer b, int offset, int header, int size) throws IOException {
    int count=b.getInt(offset+8), flags=b.getInt(offset+16), start=b.getInt(offset+20);
    if (count<0 || count>(size-header)/4) throw new IOException("Invalid string count");
    boolean utf8=(flags & 256)!=0; List<String> result=new ArrayList<>();
    for (int i=0;i<count;i++) {
      int[] p={offset+start+b.getInt(offset+header+i*4)};
      int n=length(b,p,utf8); if (utf8) n=length(b,p,true); else n=Math.multiplyExact(n,2);
      if (n<0 || p[0]<offset || p[0]+(long)n>offset+size) throw new IOException("Invalid manifest string");
      result.add(new String(b.array(),p[0],n,utf8 ? StandardCharsets.UTF_8 : StandardCharsets.UTF_16LE));
    }
    return result;
  }
}
