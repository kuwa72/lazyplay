#include "audio_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>
#include <cstring>

AacEldDecoder::AacEldDecoder() {}

AacEldDecoder::~AacEldDecoder() {
    Shutdown();
}

bool AacEldDecoder::Init() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) return false;
    m_codec = const_cast<void*>(static_cast<const void*>(codec));

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;
    m_ctx = ctx;

    // AudioSpecificConfig for AAC-ELD 44100 Hz stereo (same value UxPlay uses).
    // FFmpeg's native AAC decoder needs the ASC in extradata to select ELD.
    static const uint8_t asc[] = { 0xF8, 0xE8, 0x50, 0x00 };
    ctx->extradata = (uint8_t*)av_mallocz(sizeof(asc) + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->extradata) {
        Shutdown();
        return false;
    }
    memcpy(ctx->extradata, asc, sizeof(asc));
    ctx->extradata_size = static_cast<int>(sizeof(asc));

    ctx->sample_rate = 44100;
    av_channel_layout_default(&ctx->ch_layout, 2);

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        Shutdown();
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        Shutdown();
        return false;
    }
    m_frame = frame;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        Shutdown();
        return false;
    }
    m_pkt = pkt;

    return true;
}

void AacEldDecoder::Shutdown() {
    if (m_pkt) {
        AVPacket* pkt = static_cast<AVPacket*>(m_pkt);
        av_packet_free(&pkt);
        m_pkt = pkt;
    }
    if (m_frame) {
        AVFrame* frame = static_cast<AVFrame*>(m_frame);
        av_frame_free(&frame);
        m_frame = frame;
    }
    if (m_ctx) {
        AVCodecContext* ctx = static_cast<AVCodecContext*>(m_ctx);
        avcodec_free_context(&ctx);
        m_ctx = ctx;
    }
    m_codec = nullptr;
}

bool AacEldDecoder::Decode(const uint8_t* frame, size_t len, std::vector<int16_t>& pcm) {
    if (!m_ctx || !m_pkt || !m_frame || !frame || len == 0) return false;

    AVCodecContext* ctx = static_cast<AVCodecContext*>(m_ctx);
    AVPacket* pkt = static_cast<AVPacket*>(m_pkt);
    AVFrame* out = static_cast<AVFrame*>(m_frame);

    pkt->data = const_cast<uint8_t*>(frame);
    pkt->size = static_cast<int>(len);

    int ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) return false;

    ret = avcodec_receive_frame(ctx, out);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return false;
    if (ret < 0) return false;

    // FFmpeg native AAC decoder outputs AV_SAMPLE_FMT_FLTP (planar float).
    if (out->format != AV_SAMPLE_FMT_FLTP) return false;

    int nb_samples = out->nb_samples;
    int channels = ctx->ch_layout.nb_channels;
    if (channels <= 0 || nb_samples <= 0) return false;

    pcm.resize(nb_samples * channels);
    for (int i = 0; i < nb_samples; ++i) {
        for (int c = 0; c < channels; ++c) {
            float s = reinterpret_cast<float*>(out->data[c])[i];
            if (s < -1.0f) s = -1.0f;
            else if (s > 1.0f) s = 1.0f;
            pcm[i * channels + c] = static_cast<int16_t>(std::round(s * 32767.0f));
        }
    }
    return true;
}
