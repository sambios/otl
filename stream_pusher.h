#ifndef STREAM_PUSHER_H
#define STREAM_PUSHER_H

#include <iostream>
#include <thread>
#include <chrono>
#include <cassert>
#include <list>
#include <mutex>
#include "otl_ffmpeg.h"

namespace otl {

    class FfmpegOutputer : public FfmpegGlobal {
        enum State {
            Init = 0, Service, Down
        };
        AVFormatContext *m_ofmtCtx{nullptr};
        std::string m_url;

        std::thread *m_threadOutput{nullptr};
        bool m_threadOutputIsRunning{false};

        State m_outputState;
        std::mutex m_listPacketsLock;
        std::list<AVPacket *> m_listPackets;
        bool m_repeat{true};

        int64_t m_globalPts{0};
        int64_t m_lastPts{0};
        int64_t m_ptsBase{0};

        bool stringStartWith(const std::string &s, const std::string &prefix) {
            return (s.compare(0, prefix.size(), prefix) == 0);
        }

        int outputInitialize() {
            int ret = 0;
            if (!(m_ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
                ret = avio_open(&m_ofmtCtx->pb, m_url.c_str(), AVIO_FLAG_WRITE);
                if (ret < 0) {
                    printf("Could not open output URL '%s'", m_url.c_str());
                    return -1;
                }
            }

            AVDictionary *opts = NULL;
            if (stringStartWith(m_url, "rtsp://")) {
                av_dict_set(&opts, "rtsp_transport", "tcp", 0);
                av_dict_set(&opts, "muxdelay", "0.1", 0);
            }

            //Write file header
            ret = avformat_write_header(m_ofmtCtx, &opts);
            if (ret < 0) {
                char tmp[256];
                printf("avformat_write_header err=%s\n", av_make_error_string(tmp, sizeof(tmp), ret));
                return -1;
            }

            m_outputState = Service;
            return 0;
        }

        void outputService() {
            int ret = 0;
            while (m_listPackets.size() > 0) {
                m_listPacketsLock.lock();
                AVPacket *pkt = m_listPackets.front();
                m_listPackets.pop_front();
                m_listPacketsLock.unlock();
                if (pkt->dts != AV_NOPTS_VALUE && m_lastPts > pkt->dts)
                {
                    m_ptsBase = pkt->pts;
                }
                m_globalPts +=  pkt->pts - m_ptsBase;
                pkt->dts = pkt->pts = m_globalPts;
                m_lastPts = pkt->pts;
                ret = av_interleaved_write_frame(m_ofmtCtx, pkt);
                av_packet_free(&pkt);
                if (ret != 0) {
                    std::cout << "av_interleaved_write_frame err" << ret << std::endl;
                    // m_output_state = DOWN;
                    // break;
                }
            }

        }

        void outputDown() {
            if (m_repeat) {
                m_outputState = Init;
            } else {
                av_write_trailer(m_ofmtCtx);
                if (!(m_ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
                    avio_closep(&m_ofmtCtx->pb);
                }

                // Set exit flag
                m_threadOutputIsRunning = false;
            }
        }

        void outputProcessThreadProc() {
            m_threadOutputIsRunning = true;
            while (m_threadOutputIsRunning) {
                switch (m_outputState) {
                    case Init:
                        if (outputInitialize() < 0) m_outputState = Down;
                        break;
                    case Service:
                        outputService();
                        break;
                    case Down:
                        outputDown();
                        break;
                }
            }

            std::cout << "output thread exit!" << std::endl;
        }

    public:
        FfmpegOutputer() : m_ofmtCtx(NULL) {

        }

        virtual ~FfmpegOutputer() {
            closeOutputStream();
        }

        int openOutputStream(const std::string &url, const AVFormatContext *ifmtCtx) {
            int ret = 0;
            const char *formatName = NULL;
            m_url = url;

            if (stringStartWith(m_url, "rtsp://")) {
                formatName = "rtsp";
            } else if (stringStartWith(m_url, "udp://") || stringStartWith(m_url, "tcp://")) {
                if (ifmtCtx && ifmtCtx->streams[0]->codecpar->codec_id == AV_CODEC_ID_H264)
                    formatName = "h264";
                else if(ifmtCtx && ifmtCtx->streams[0]->codecpar->codec_id == AV_CODEC_ID_HEVC)
                    formatName = "hevc";
                else
                    formatName = "rawvideo";
            } else if (stringStartWith(m_url, "rtp://")) {
                formatName = "rtp";
            } else if (stringStartWith(m_url, "rtmp://")) {
                formatName = "flv";
            } else {
                std::cout << "Not support this Url:" << m_url << std::endl;
                return -1;
            }

            std::cout << "open url=" << m_url << ",format_name=" << formatName << std::endl;

            if (nullptr == m_ofmtCtx) {
                ret = avformat_alloc_output_context2(&m_ofmtCtx, NULL, formatName, m_url.c_str());
                if (ret < 0 || m_ofmtCtx == NULL) {
                    std::cout << "avformat_alloc_output_context2() err=" << ret << std::endl;
                    return -1;
                }

                for (int i = 0; i < 1; ++i) {
                    AVStream *ostream = avformat_new_stream(m_ofmtCtx, NULL);
                    if (NULL == ostream) {
                        std::cout << "Can't create new stream!" << std::endl;
                        return -1;
                    }

                    if (ifmtCtx) {
#if LIBAVCODEC_VERSION_MAJOR > 56
                        ret = avcodec_parameters_copy(ostream->codecpar, ifmtCtx->streams[i]->codecpar);
                        if (ret < 0) {
                            std::cout << "avcodec_parameters_copy() err=" << ret << std::endl;
                            return -1;
                        }
#else
                        ret = avcodec_copy_context(ostream->codec, ifmtCtx->streams[i]->codec);
                        if (ret < 0){
                            printf("avcodec_copy_context() err=%d", ret);
                            return -1;
                        }
#endif
                    }
                }

                m_ofmtCtx->oformat->flags |= AVFMT_TS_NONSTRICT;
            }

            if (!m_ofmtCtx) {
                printf("Could not create output context\n");
                return -1;
            }

            av_dump_format(m_ofmtCtx, 0, m_url.c_str(), 1);
            ret = outputInitialize();
            if (ret != 0) {
                return -1;
            }

            m_threadOutput = new std::thread(&FfmpegOutputer::outputProcessThreadProc, this);
            return 0;
        }


        int inputPacket(const AVPacket *pkt) {
            AVPacket *pkt1 = av_packet_alloc();
            av_packet_ref(pkt1, pkt);
            m_listPacketsLock.lock();
            m_listPackets.push_back(pkt1);
            m_listPacketsLock.unlock();
            return 0;
        }

        int closeOutputStream() {
            std::cout << "call CloseOutputStream()" << std::endl;
            m_repeat = false;
            m_outputState = Down;
            if (m_threadOutput) {
                m_threadOutput->join();
                delete m_threadOutput;
                m_threadOutput = nullptr;
            }

            if (m_ofmtCtx) {
                avformat_free_context(m_ofmtCtx);
                m_ofmtCtx = NULL;
            }

            return 0;
        }
    };
} // namespace otl

#endif // STREAM_PUSHER_H
