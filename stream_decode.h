
#ifndef STREAM_DECODE_H
#define STREAM_DECODE_H

#include "stream_demuxer.h"

namespace otl {

#if LIBAVCODEC_VERSION_MAJOR <= 56
static AVPacket *ffmpeg_packet_alloc() {
    AVPacket* pkt = new AVPacket;
    av_init_packet(pkt);
    return pkt;
}

static void ffmpeg_packet_free(AVPacket** pkt){
    av_free_packet(*pkt);
    av_freep(pkt);
}
#endif

struct StreamDecoderEvents {
    virtual ~StreamDecoderEvents() {}
    virtual void onDecodedAVFrame(const AVPacket *pkt, const AVFrame *pFrame) = 0;
    virtual void onDecodedSeiInfo(const uint8_t *sei_data, int sei_data_len, uint64_t pts, int64_t pkt_pos) {};
    virtual void onStreamEof() {};
};

class StreamDecoder : public StreamDemuxerEvents {
    StreamDecoderEvents *m_observer;

    using OnDecodedFrameCallback = std::function<void(const AVPacket *pkt, const AVFrame *pFrame)>;
    using OnDecodedSEICallback = std::function<void(const uint8_t *sei_data, int sei_data_len, uint64_t pts, int64_t pkt_pos)>;
    using OnStreamEofCallback = std::function<void()>;
    OnDecodedFrameCallback m_onDecodedFrameFunc;
    OnDecodedSEICallback m_onDecodedSEIFunc;

    StreamDemuxer::OnAvformatOpenedFunc m_pfnOnAVFormatOpened;
    StreamDemuxer::OnAvformatClosedFunc m_pfnOnAVFormatClosed;
    StreamDemuxer::OnReadFrameFunc m_pfnOnReadFrame;
    StreamDemuxer::OnReadEofFunc m_pfnOnReadEof;

protected:
    std::list<AVPacket *> m_listPackets;
    AVCodecContext *m_decCtx{nullptr};
    AVCodecContext *m_externalDecCtx{nullptr};
    int m_videoStreamIndex{0};
    int m_frameDecodedNum{0};
    StreamDemuxer m_demuxer;
    AVDictionary *m_optsDecoder{nullptr};
    bool m_isWaitingIframe{true};
    int m_id{0};
    AVRational m_timebase;

    int createVideoDecoder(AVFormatContext *ifmtCtx);
    int putPacket(AVPacket *pkt);
    AVPacket *getPacket();
    void clearPackets();
    int decodeFrame(AVPacket *pkt, AVFrame *pFrame);
    int getVideoStreamIndex(AVFormatContext *ifmtCtx);
    bool isKeyFrame(AVPacket *pkt);

    // Overload StreamDemuxerEvents Interface.
    virtual void onAvformatOpened(AVFormatContext *ifmtCtx) override;
    virtual void onAvformatClosed() override;
    virtual int onReadFrame(AVPacket *pkt) override;
    virtual void onReadEof(AVPacket *pkt) override;

public:
    StreamDecoder(int id, AVCodecContext *decoder = nullptr);
    virtual ~StreamDecoder();

    int setObserver(StreamDecoderEvents *observer);

    void setDecodedFrameCallback(OnDecodedFrameCallback func) {
        m_onDecodedFrameFunc = func;
    }

    void setDecodedSeiInfoCallback(OnDecodedSEICallback func) {
        m_onDecodedSEIFunc = func;
    }

    void setAvformatOpenedCallback(StreamDemuxer::OnAvformatOpenedFunc func) {
        m_pfnOnAVFormatOpened = func;
    }

    void setAvformatClosedCallback(StreamDemuxer::OnAvformatClosedFunc func) {
        m_pfnOnAVFormatClosed = func;
    }

    void setReadFrameCallback(StreamDemuxer::OnReadFrameFunc func) {
        m_pfnOnReadFrame = func;
    }

    void setReadEofCallback(StreamDemuxer::OnReadEofFunc func) {
        m_pfnOnReadEof = func;
    }

    int openStream(std::string url, bool repeat = true, AVDictionary *opts = nullptr);
    int closeStream(bool isWaiting = true);
    AVCodecID getVideoCodecId();

    // External utilities
    static AVPacket* ffmpegPacketAlloc();
    static AVCodecContext* ffmpegCreateDecoder(enum AVCodecID id, AVDictionary **opts = nullptr);
};

} // namespace otl

#endif // STREAM_DECODE_H
