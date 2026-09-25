package vendor.matonos.channel;

import vendor.matonos.channel.IChannelListener;

@VintfStability
interface IChannel {
    String call(String command, String jsonArgs);
    void subscribe(String topic, IChannelListener listener);
    void unsubscribe(String topic, IChannelListener listener);
}
