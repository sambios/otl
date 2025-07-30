#include "stream_demuxer.h"

namespace otl {

StreamDemuxer::StreamDemuxer(int id)
    : m_ifmtCtx(nullptr), m_observer(nullptr), m_threadReading(nullptr), m_id(id) {
    m_ifmtCtx = avformat_alloc_context();
}

StreamDemuxer::~StreamDemuxer() {
    std::cout << "~StreamDemuxer() dtor..." << std::endl;
    closeStream(false);
    avformat_close_input(&m_ifmtCtx);
    avformat_free_context(m_ifmtCtx);
}

int StreamDemuxer::doInitialize() {
    std::string prefix = "rtsp://";
    AVDictionary *opts = nullptr;
    if (m_inputUrl.compare(0, prefix.size(), prefix) == 0) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "2000000", 0);
        av_dict_set(&opts, "probesize", "400", 0);
        av_dict_set(&opts, "analyzeduration", "100", 0);
    } else {
        m_isFileUrl = true;
    }

    av_dict_set(&opts, "rw_timeout", "15000", 0);

    std::cout << "Open stream " << m_inputUrl << std::endl;

    int ret = avformat_open_input(&m_ifmtCtx, m_inputUrl.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        std::cout << "Can't open file " << m_inputUrl << std::endl;
        return ret;
    }

    ret = avformat_find_stream_info(m_ifmtCtx, nullptr);
    if (ret < 0) {
        std::cout << "Unable to get stream info" << std::endl;
        return ret;
    }

    std::cout << "Init:total stream num:" << m_ifmtCtx->nb_streams << std::endl;
    if (m_observer) {
        m_observer->onAvformatOpened(m_ifmtCtx);
    }

    if (m_pfnOnAVFormatOpened != nullptr) {
        m_pfnOnAVFormatOpened(m_ifmtCtx);
    }

    m_workState = State::Service;
    return 0;
}

int StreamDemuxer::doDown() {
    avformat_close_input(&m_ifmtCtx);

    if (m_observer) {
        m_observer->onAvformatClosed();
    }

    if (m_pfnOnAVFormatClosed != nullptr) {
        m_pfnOnAVFormatClosed();
    }

    if (m_repeat) {
        m_workState = State::Initialize;
    } else {
        m_keepRunning = false;
    }

    return 0;
}

int StreamDemuxer::doService() {
#if LIBAVCODEC_VERSION_MAJOR > 56
    AVPacket *pkt = av_packet_alloc();
#else
    AVPacket *pkt = (AVPacket*)av_malloc(sizeof(AVPacket));
    av_init_packet(pkt);
#endif

    m_startTime = av_gettime();
    int64_t frameIndex = 0;
    while (State::Service == m_workState) {
        int ret = av_read_frame(m_ifmtCtx, pkt);
        if (ret < 0) {
            if (ret != AVERROR_EOF) continue;
            if (m_repeat && m_isFileUrl) {
                ret = av_seek_frame(m_ifmtCtx, -1, m_ifmtCtx->start_time, 0);
                if (ret != 0) {
                    ret = av_seek_frame(m_ifmtCtx, -1, m_ifmtCtx->start_time, AVSEEK_FLAG_BYTE);
                    if (ret < 0) {
                        std::cout << "av_seek_frame failed!" << std::endl;
                    }
                }
                frameIndex = 0;
                m_startTime = av_gettime();
                printf("seek_to_start\n");
                continue;
            } else {
                printf("file[%d] end!\n", m_id);
                if (m_observer) m_observer->onReadEof(pkt);
                if (m_pfnOnReadEof != nullptr) m_pfnOnReadEof(pkt);
                m_workState = State::Down;
            }
            break;
        }

        if (m_lastFrameTime != 0) {
            if (pkt->pts == AV_NOPTS_VALUE) {
                AVRational timeBase1 = m_ifmtCtx->streams[0]->time_base;
                int64_t calcDuration = (double)AV_TIME_BASE / av_q2d(m_ifmtCtx->streams[0]->r_frame_rate);
                pkt->pts = (double)(frameIndex * calcDuration) / (double)(av_q2d(timeBase1) * AV_TIME_BASE);
                pkt->dts = pkt->pts;
                pkt->duration = (double)calcDuration / (double)(av_q2d(timeBase1) * AV_TIME_BASE);
            }

            AVRational timeBase = m_ifmtCtx->streams[0]->time_base;
            AVRational timeBaseQ = {1, AV_TIME_BASE};
            int64_t ptsTime = av_rescale_q(pkt->dts, timeBase, timeBaseQ);
            int64_t nowTime = av_gettime() - m_startTime;
            if (ptsTime > nowTime) {
                int64_t delta = ptsTime - nowTime;
                if (delta < 100000) {
                    av_usleep(delta);
                }
            }
        }

        m_lastFrameTime = av_gettime();
        if (pkt->stream_index == 0) frameIndex++;

        if (m_observer) {
            m_observer->onReadFrame(pkt);
        }

        if (m_pfnOnReadFrame) {
            m_pfnOnReadFrame(pkt);
        }

        av_packet_unref(pkt);
    }

#if LIBAVCODEC_VERSION_MAJOR > 56
    av_packet_free(&pkt);
#else
    av_free_packet(pkt);
    av_freep(&pkt);
#endif

    return 0;
}

int StreamDemuxer::openStream(const std::string& url, StreamDemuxerEvents *observer, bool repeat, bool isSyncOpen) {

    closeStream(false);

    m_inputUrl = url;
    m_observer = observer;
    m_repeat = repeat;
    m_workState = State::Initialize;
    if (isSyncOpen) {
        int ret = doInitialize();
        if (ret < 0) {
            return ret;
        }
    }

    m_keepRunning = true;
    m_threadReading = new std::thread([&] {
        while (m_keepRunning) {
            switch (m_workState) {
                case State::Initialize:
                    if (doInitialize() != 0) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    break;
                case State::Service:
                    doService();
                    break;
                case State::Down:
                    doDown();
                    break;
            }
        }
    });

    return 0;
}

int StreamDemuxer::closeStream(bool isWaiting) {
    if (!isWaiting) {
        m_workState = State::Down;
        m_repeat = false;
    }

    if (nullptr != m_threadReading) {
        m_threadReading->join();
        delete m_threadReading;
        m_threadReading = nullptr;
    }

    return 0;
}

} // namespace otl
