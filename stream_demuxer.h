#ifndef STREAM_DEMUXER_H
#define STREAM_DEMUXER_H

#include <iostream>
#include <thread>
#include <list>
#include <functional>
#include "otl_ffmpeg.h"

namespace otl {

struct StreamDemuxerEvents {
    virtual void onAvformatOpened(AVFormatContext *ifmtCtx) {}
    virtual void onAvformatClosed() {}
    virtual int onReadFrame(AVPacket *pkt) = 0;
    virtual void onReadEof(AVPacket *pkt) = 0;
};

class StreamDemuxer : public FfmpegGlobal {
public:
    enum class State : int8_t {
        Initialize = 0,
        Service,
        Down
    };

    using OnAvformatOpenedFunc = std::function<void(AVFormatContext*)>;
    using OnAvformatClosedFunc = std::function<void()>;
    using OnReadFrameFunc = std::function<void(AVPacket *)>;
    using OnReadEofFunc = std::function<void(AVPacket *)>;
private:
    AVFormatContext *m_ifmtCtx;
    StreamDemuxerEvents *m_observer;
    State m_workState;
    std::string m_inputUrl;
    std::thread *m_threadReading;
    bool m_repeat;
    bool m_keepRunning;
    int64_t m_lastFrameTime{0};
    int64_t m_startTime;
    bool m_isFileUrl{false};
    int m_id;

    OnAvformatOpenedFunc m_pfnOnAVFormatOpened;
    OnAvformatClosedFunc m_pfnOnAVFormatClosed;
    OnReadFrameFunc m_pfnOnReadFrame;
    OnReadEofFunc m_pfnOnReadEof;
protected:
    int doInitialize();
    int doService();
    int doDown();

public:
    StreamDemuxer(int id = 0);
    virtual ~StreamDemuxer();

    void setAvformatOpenedCallback(OnAvformatOpenedFunc func) { m_pfnOnAVFormatOpened = func; }
    void setAvformatClosedCallback(OnAvformatClosedFunc func) { m_pfnOnAVFormatClosed = func; }
    void setReadFrameCallback(OnReadFrameFunc func) { m_pfnOnReadFrame = func; }
    void setReadEofCallback(OnReadEofFunc func) { m_pfnOnReadEof = func; }

    int openStream(const std::string &url, StreamDemuxerEvents *observer, bool repeat = true, bool isSyncOpen = false);
    int closeStream(bool isWaiting);
};

} // namespace otl

#endif // STREAM_DEMUXER_H
