#ifndef STREAM_PUSHER_H
#define STREAM_PUSHER_H

#include <iostream>
#include <thread>
#include <chrono>
#include <cassert>
#include <numeric>
#include <list>
#include <mutex>
#include "otl_ffmpeg.h"

namespace otl {

    class TimestampCorrector {
    private:
        // 修正后的历史时间戳（严格单调递增）
        std::vector<double> corrected_history_;
        // 历史时间间隔（用于计算趋势）
        std::vector<double> deltas_;
        // 最近N个间隔用于计算参考趋势（窗口大小）
        size_t window_size_;
        // 最小时间间隔（确保严格递增）
        double epsilon_;
        // 对原始时间戳的信任因子（0~1，越大越依赖原始值）
        double trust_factor_;

    public:
        /**
         * 构造函数
         * @param window_size 计算历史趋势的窗口大小
         * @param epsilon 最小时间间隔
         * @param trust_factor 对原始时间戳的信任度（0~1）
         */
        TimestampCorrector(
            size_t window_size = 5,
            double epsilon = 1e-6,
            double trust_factor = 0.7
        ) : window_size_(window_size), epsilon_(epsilon), trust_factor_(trust_factor) {
            if (trust_factor < 0 || trust_factor > 1) {
                throw std::invalid_argument("trust_factor must be in [0, 1]");
            }
            if (epsilon <= 0) {
                throw std::invalid_argument("epsilon must be positive");
            }
        }

        /**
         * 修正新到来的时间戳
         * @param raw_timestamp 新帧的原始时间戳
         * @return 修正后的时间戳
         */
        double correct(double raw_timestamp) {
            if (corrected_history_.empty()) {
                // 第一帧：直接使用原始时间戳作为起点
                corrected_history_.push_back(raw_timestamp);
                return raw_timestamp;
            }

            // 历史最后一帧的时间戳（当前帧必须大于此值）
            double last_corrected = corrected_history_.back();

            // 1. 计算历史趋势：基于最近window_size个间隔的平均值
            double ref_delta = epsilon_; // 默认最小间隔
            if (!deltas_.empty()) {
                // 取最近的window_size个间隔（或全部间隔）
                size_t start = std::max(0LL, static_cast<long long>(deltas_.size()) - static_cast<long long>(window_size_));
                std::vector<double> recent_deltas(deltas_.begin() + start, deltas_.end());

                // 计算平均间隔作为参考趋势
                ref_delta = std::accumulate(recent_deltas.begin(), recent_deltas.end(), 0.0)
                          / recent_deltas.size();
                ref_delta = std::max(ref_delta, epsilon_); // 确保参考间隔为正数
            }

            // 2. 预测合理的时间戳范围（基于历史趋势）
            double predicted_min = last_corrected + epsilon_;          // 最小可能值（必须大于上一帧）
            double predicted = last_corrected + ref_delta;             // 基于历史的预测值
            double predicted_max = predicted + 2 * ref_delta;          // 最大合理值（允许一定波动）

            // 3. 根据原始时间戳的合理性进行修正
            double corrected;
            if (raw_timestamp > predicted_min) {
                if (raw_timestamp <= predicted_max) {
                    // 原始值在合理范围内：加权融合原始值和预测值
                    corrected = trust_factor_ * raw_timestamp + (1 - trust_factor_) * predicted;
                } else {
                    // 原始值偏大：限制在合理最大值附近
                    corrected = predicted_max;
                }
            } else {
                // 原始值偏小（<= 上一帧）：使用历史预测值
                corrected = predicted;
            }

            // 最终确保严格递增（防御性处理）
            corrected = std::max(corrected, last_corrected + epsilon_);

            // 4. 更新历史数据
            corrected_history_.push_back(corrected);
            deltas_.push_back(corrected - last_corrected);

            return corrected;
        }

        /**
         * 获取修正后的历史时间戳
         */
        const std::vector<double>& get_corrected_history() const {
            return corrected_history_;
        }

        /**
         * 重置修正器（清空历史数据）
         */
        void reset() {
            corrected_history_.clear();
            deltas_.clear();
        }
    };

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
        int64_t m_lastGlobalPts{0};
        int64_t m_lastPts{0};
        int64_t m_ptsBase{0};
        TimestampCorrector m_tsAdjust;

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

                if (pkt->dts != AV_NOPTS_VALUE && m_globalPts >= pkt->dts)
                {
                    //m_globalPts =  m_ptsBase + m_lastPts - pkt->pts;
                    //pkt->dts = pkt->pts = m_globalPts;
                }

                pkt->dts = m_tsAdjust.correct(pkt->dts);
                pkt->pts = pkt->dts;
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

                // >> fixed
                // m_ofmtCtx->oformat->flags |= AVFMT_TS_NONSTRICT;
                // << fixed
                // 如果你需要设置格式相关的选项，可以使用 av_opt_set 等函数
                // 例如设置严格度（如果需要非严格的时间戳）
                av_opt_set(m_ofmtCtx, "strict", "experimental", 0);
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
