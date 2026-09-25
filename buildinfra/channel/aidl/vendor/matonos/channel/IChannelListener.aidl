package vendor.matonos.channel;

@VintfStability
oneway interface IChannelListener {
    void onEvent(String topic, String json);
}
