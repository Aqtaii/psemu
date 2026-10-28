// Independent decoder check, before the AvPlayer PCM converter.
#include <filesystem>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}
static bool RawAudioNonzero(const char *path, uint64_t &blocks, uint64_t &nonzero) {
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0)
        return false;
    struct FormatOwner {
        AVFormatContext *&p;
        ~FormatOwner() { avformat_close_input(&p); }
    } fo{format};
    if (avformat_find_stream_info(format, nullptr) < 0)
        return false;
    int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream < 0)
        return false;
    auto *codec = avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context)
        return false;
    struct CodecOwner {
        AVCodecContext *&p;
        ~CodecOwner() { avcodec_free_context(&p); }
    } co{context};
    if (avcodec_parameters_to_context(context, format->streams[stream]->codecpar) < 0 ||
        avcodec_open2(context, codec, nullptr) < 0)
        return false;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct PacketOwner {
        AVPacket *&p;
        AVFrame *&f;
        ~PacketOwner() {
            av_packet_free(&p);
            av_frame_free(&f);
        }
    } po{packet, frame};
    if (!packet || !frame)
        return false;
    const auto receive = [&] {
        while (avcodec_receive_frame(context, frame) >= 0) {
            ++blocks;
            const auto fmt = static_cast<AVSampleFormat>(frame->format);
            const bool planar = av_sample_fmt_is_planar(fmt);
            const int channels = frame->ch_layout.nb_channels;
            const int count = frame->nb_samples * (planar ? 1 : channels);
            for (int c = 0; c < (planar ? channels : 1); ++c) {
                const auto *data = frame->extended_data[c];
                for (int i = 0; i < count; ++i) {
                    switch (av_get_packed_sample_fmt(fmt)) {
                    case AV_SAMPLE_FMT_FLT:
                        nonzero += reinterpret_cast<const float *>(data)[i] != 0;
                        break;
                    case AV_SAMPLE_FMT_DBL:
                        nonzero += reinterpret_cast<const double *>(data)[i] != 0;
                        break;
                    case AV_SAMPLE_FMT_S16:
                        nonzero += reinterpret_cast<const int16_t *>(data)[i] != 0;
                        break;
                    default:
                        return false;
                    }
                }
            }
            av_frame_unref(frame);
        }
        return true;
    };
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream) {
            if (avcodec_send_packet(context, packet) < 0 || !receive())
                return false;
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(context, nullptr);
    return receive() && blocks > 0;
}