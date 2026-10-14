package org.matonos.systembridge;

import java.net.IDN;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.Map;

/** Strict subset of flatpakrepo: never forward caller-supplied Flatpak options. */
final class FlatpakRepo {
    final String title, url, host, key, fingerprint, name, canonical;
    FlatpakRepo(byte[] bytes) throws Exception {
        if (bytes == null || bytes.length == 0 || bytes.length > 65536)
            throw new IllegalArgumentException("Repository file is missing or too large");
        String text=StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
        Map<String,String> values=new LinkedHashMap<>(); boolean group=false;
        for(String raw:text.split("\n",-1)) {
            String line=raw.trim();
            if(line.isEmpty() || line.startsWith("#") || line.startsWith(";"))continue;
            if(line.equals("[Flatpak Repo]") && !group){group=true;continue;}
            int at=line.indexOf('=');
            if(!group || at<1 || line.startsWith("["))throw new IllegalArgumentException("Malformed repository file");
            String field=line.substring(0,at).trim(), value=line.substring(at+1).trim();
            if(!java.util.Set.of("Title","Url","GPGKey","Homepage","Comment","Icon","Version").contains(field)
                    || values.put(field,value)!=null || value.chars().anyMatch(c->c<32 || c==127 || Character.getType(c)==Character.FORMAT || c==92))
                throw new IllegalArgumentException("Invalid or duplicate repository field");
        }
        title=values.get("Title");key=values.get("GPGKey");
        if(title==null || title.isEmpty() || title.length()>256 || key==null || key.isEmpty())
            throw new IllegalArgumentException("A title and GPG signing key are required");
        URI input=new URI(values.getOrDefault("Url",""));
        String authority=input.getRawAuthority();
        if(!"https".equalsIgnoreCase(input.getScheme()) || authority==null || authority.contains("@")
                || input.getRawFragment()!=null || input.getRawQuery()!=null || authority.contains("\\")
                || authority.contains("%"))throw new IllegalArgumentException("Repository URL must use HTTPS");
        String hostname=input.getHost();int port=input.getPort();
        if(hostname==null) {
            if(authority.contains(":"))throw new IllegalArgumentException("Invalid repository host");
            hostname=authority;
        }
        host=IDN.toASCII(hostname,IDN.USE_STD3_ASCII_RULES).toLowerCase(java.util.Locale.ROOT);
        if(host.isEmpty() || host.length()>253 || port>65535)throw new IllegalArgumentException("Invalid repository host");
        String path=input.getRawPath();
        url="https://"+host+(port==-1 || port==443?"":":"+port)+(path==null || path.isEmpty()?"/":path);
        fingerprint=fingerprints(Base64.getDecoder().decode(key));
        byte[] digest=MessageDigest.getInstance("SHA-256").digest(url.getBytes(StandardCharsets.UTF_8));
        name="repo-"+hex(digest).substring(0,24);
        StringBuilder normalized=new StringBuilder("[Flatpak Repo]\nTitle="+title+"\nUrl="+url+"\nGPGKey="+key+"\n");
        for(String field:new String[]{"Homepage","Comment","Icon"})
            if(values.containsKey(field))normalized.append(field).append('=').append(values.get(field)).append('\n');
        canonical=normalized.toString();
        if(canonical.getBytes(StandardCharsets.UTF_8).length>65536)throw new IllegalArgumentException("Normalized repository is too large");
    }
    private static String hex(byte[] bytes) {
        StringBuilder out=new StringBuilder();for(byte b:bytes)out.append(String.format(java.util.Locale.ROOT,"%02X",b&255));return out.toString();
    }
    /** Fingerprint the actual primary public-key packets, not the key file. */
    static String fingerprints(byte[] bytes) throws Exception {
        StringBuilder out=new StringBuilder();int pos=0;
        while(pos<bytes.length) {
            int header=bytes[pos++]&255,tag,length;
            if((header&128)==0)throw new IllegalArgumentException("Invalid OpenPGP packet");
            if((header&64)!=0) {
                tag=header&63;if(pos>=bytes.length)throw new IllegalArgumentException("Truncated key");
                int n=bytes[pos++]&255;
                if(n<192)length=n;
                else if(n<224){if(pos>=bytes.length)throw new IllegalArgumentException("Truncated key");length=((n-192)<<8)+(bytes[pos++]&255)+192;}
                else if(n==255){if(pos+4>bytes.length)throw new IllegalArgumentException("Truncated key");long v=0;for(int i=0;i<4;i++)v=(v<<8)|(bytes[pos++]&255);if(v>65536)throw new IllegalArgumentException("Oversized packet");length=(int)v;}
                else throw new IllegalArgumentException("Partial key packets refused");
            } else {
                tag=(header>>2)&15;int n=header&3;if(n==3)throw new IllegalArgumentException("Indeterminate packet refused");
                int count=1<<n;if(pos+count>bytes.length)throw new IllegalArgumentException("Truncated key");long v=0;
                for(int i=0;i<count;i++)v=(v<<8)|(bytes[pos++]&255);if(v>65536)throw new IllegalArgumentException("Oversized packet");length=(int)v;
            }
            if(length<1 || pos+length>bytes.length || tag==5 || tag==7)throw new IllegalArgumentException("Invalid public key");
            if(tag==6) {
                int version=bytes[pos]&255;MessageDigest digest;
                if(version==4) {digest=MessageDigest.getInstance("SHA-1");digest.update((byte)0x99);digest.update((byte)(length>>8));digest.update((byte)length);}
                else if(version==5 || version==6) {digest=MessageDigest.getInstance("SHA-256");digest.update((byte)(version==5?0x9a:0x9b));for(int i=3;i>=0;i--)digest.update((byte)(length>>(i*8)));}
                else throw new IllegalArgumentException("Unsupported key version");
                digest.update(bytes,pos,length);if(out.length()>0)out.append("\n");out.append(hex(digest.digest()));
            }
            pos+=length;
        }
        if(out.length()==0)throw new IllegalArgumentException("No primary public key");return out.toString();
    }
}
