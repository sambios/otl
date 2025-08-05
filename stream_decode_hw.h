
#ifndef STREAM_DECODE_HW_H
#define STREAM_DECODE_HW_H

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
    StreamDecoderEvents *mObserver;

    using OnDecodedFrameCallback = std::function<void(const AVPacket *pkt, const AVFrame *pFrame)>;
    using OnDecodedSeiCallback = std::function<void(const uint8_t *seiData, int seiDataLen, uint64_t pts, int64_t pktPos)>;
    using OnStreamEofCallback = std::function<void()>;
    OnDecodedFrameCallback mOnDecodedFrameFunc;
    OnDecodedSeiCallback mOnDecodedSeiFunc;

    StreamDemuxer::OnAvformatOpenedFunc mOnAvformatOpenedFunc;
    StreamDemuxer::OnAvformatClosedFunc mOnAvformatClosedFunc;
    StreamDemuxer::OnReadFrameFunc mOnReadFrameFunc;
    StreamDemuxer::OnReadEofFunc mOnReadEofFunc;

protected:
    std::list<AVPacket *> mListPackets;
    AVCodecContext *mDecCtx{nullptr};
    AVCodecContext *mExternalDecCtx{nullptr};
    int mVideoStreamIndex{0};
    int mFrameDecodedNum{0};
    StreamDemuxer mDemuxer;
    AVDictionary *mOptsDecoder{nullptr};
    bool mIsWaitingIframe{true};
    int mId{0};
    AVRational mTimebase;

    int createVideoDecoder(AVFormatContext *ifmtCtx);
    int putPacket(AVPacket *pkt);
    AVPacket *getPacket();
    void clearPackets();
    int decodeFrame(AVPacket *pkt, AVFrame *pFrame);
    int getVideoStreamIndex(AVFormatContext *ifmtCtx);
    bool isKeyFrame(AVPacket *pkt);

    int initHWConfig(int devId, int vpuId);

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
        mOnDecodedFrameFunc = func;
    }

    void setDecodedSeiInfoCallback(OnDecodedSeiCallback func) {
        mOnDecodedSeiFunc = func;
    }

    void setAvformatOpenedCallback(StreamDemuxer::OnAvformatOpenedFunc func) {
        mOnAvformatOpenedFunc = func;
    }

    void setAvformatClosedCallback(StreamDemuxer::OnAvformatClosedFunc func) {
        mOnAvformatClosedFunc = func;
    }

    void setReadFrameCallback(StreamDemuxer::OnReadFrameFunc func) {
        mOnReadFrameFunc = func;
    }

    void setReadEofCallback(StreamDemuxer::OnReadEofFunc func) {
        mOnReadEofFunc = func;
    }

    int openStream(std::string url, bool repeat = true, AVDictionary *opts = nullptr);
    int closeStream(bool isWaiting = true);
    AVCodecID getVideoCodecId();

    // External utilities
    static AVPacket* ffmpegPacketAlloc();
    static AVCodecContext* ffmpegCreateDecoder(enum AVCodecID id, AVDictionary **opts = nullptr);

    //HWAccels
    char mszHWDevTypeName[64];
    enum AVHWDeviceType mHWDevType;
    enum AVPixelFormat mHwPixFmt{AV_PIX_FMT_NONE};
    AVBufferRef *mHWDeviceCtx{nullptr};

};

} // namespace otl

#endif // STREAM_DECODE_HW_H
