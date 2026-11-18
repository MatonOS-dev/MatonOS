package vendor.matonos.channel;

import android.os.ParcelFileDescriptor;
import vendor.matonos.channel.IChannelListener;

@VintfStability
interface IChannel {
    String call(String command, String jsonArgs);
    void subscribe(String topic, IChannelListener listener);
    void unsubscribe(String topic, IChannelListener listener);
    String stageUpdate(in String[] names, in ParcelFileDescriptor[] files);
    String finalizeUpdate();
    String getUpdateStatus();
}
