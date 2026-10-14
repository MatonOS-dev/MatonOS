package org.matonos.systembridge;
import android.os.Parcel;
import android.os.Parcelable;
/** Installed provenance; availableCommit is empty for inventory-only queries. */
public final class LinuxAppInfo implements Parcelable {
    public String appId, remote, installedCommit, availableCommit;
    public LinuxAppInfo(String id,String origin,String installed,String available) {
        appId=id;remote=origin;installedCommit=installed;availableCommit=available;
    }
    private LinuxAppInfo(Parcel p) { this(p.readString(),p.readString(),p.readString(),p.readString()); }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel p,int flags) {
        p.writeString(appId);p.writeString(remote);p.writeString(installedCommit);p.writeString(availableCommit);
    }
    public static final Creator<LinuxAppInfo> CREATOR=new Creator<LinuxAppInfo>() {
        public LinuxAppInfo createFromParcel(Parcel p) { return new LinuxAppInfo(p); }
        public LinuxAppInfo[] newArray(int n) { return new LinuxAppInfo[n]; }
    };
}
