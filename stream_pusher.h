#ifndef STREAM_PUSHER_H
#define STREAM_PUSHER_H

#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <string>
#include "timestamp_smoother.h"
#include "otl_ffmpeg.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace otl {
namespace internal {
    template<typename T>
    class BlockingQueue {
    private:
        std::queue<T> m_queue;
        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        bool m_shutdown{false};

    public:
        void push(T item) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_shutdown) {
                m_queue.push(item);
                m_condition.notify_one();
            }
        }

        bool pop(T& item, int timeoutMs = -1) {
            std::unique_lock<std::mutex> lock(m_mutex);
            
            if (timeoutMs < 0) {
                // Wait indefinitely until item available or shutdown
                m_condition.wait(lock, [this] { return !m_queue.empty() || m_shutdown; });
            } else {
                // Wait with timeout
                if (!m_condition.wait_for(lock, std::chrono::milliseconds(timeoutMs), 
                                         [this] { return !m_queue.empty() || m_shutdown; })) {
                    return false; // Timeout
                                         }
            }
            
            if (m_shutdown && m_queue.empty()) {
                return false; // Shutdown and no more items
            }
            
            if (!m_queue.empty()) {
                item = m_queue.front();
                m_queue.pop();
                return true;
            }
            
            return false;
        }

        size_t size() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.size();
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.empty();
        }

        void shutdown() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shutdown = true;
            m_condition.notify_all();
        }

        void reset() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shutdown = false;
            // Clear any remaining items
            while (!m_queue.empty()) {
                m_queue.pop();
            }
        }
    };
}

    class FfmpegOutputer : public FfmpegGlobal {
        enum State {
            Init = 0, Service, Down
        };
        AVFormatContext *m_ofmtCtx{nullptr};
        std::string m_url;

        std::thread *m_threadOutput{nullptr};
        bool m_threadOutputIsRunning{false};

        State m_outputState;
        internal::BlockingQueue<AVPacket *> m_packetQueue;
        bool m_repeat{true};
        TimestampSmoother m_timestampSmoother;
        
        // 保留原有变量用于兼容性（已弃用）
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
            AVPacket *pkt = nullptr;
            
            // Try to get a packet with a short timeout to allow for responsive shutdown
            if (m_packetQueue.pop(pkt, 10)) { // 10ms timeout
                // 使用时间戳平滑器处理时间戳
                if (m_timestampSmoother.smoothTimestamp(pkt)) {
                    ret = av_interleaved_write_frame(m_ofmtCtx, pkt);
                    if (ret != 0) {
                        char errorBuf[256];
                        av_strerror(ret, errorBuf, sizeof(errorBuf));
                        std::cout << "av_interleaved_write_frame err: " << ret 
                                  << " (" << errorBuf << ")" << std::endl;
                        
                        // 如果是时间戳相关错误，打印统计信息
                        if (ret == AVERROR(EINVAL)) {
                            m_timestampSmoother.printStatistics();
                        }
                    }
                } else {
                    std::cout << "Failed to smooth timestamp for packet" << std::endl;
                }
                
                av_packet_free(&pkt);
            }
        }

        void outputDown() {
            if (m_repeat) {
                m_outputState = Init;
            } else {
                // Process any remaining packets before shutdown
                AVPacket *pkt = nullptr;
                while (m_packetQueue.pop(pkt, 0)) { // Non-blocking pop to drain queue
                    av_packet_free(&pkt);
                }
                
                av_write_trailer(m_ofmtCtx);
                if (!(m_ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
                    avio_closep(&m_ofmtCtx->pb);
                }

                // Shutdown the queue and set exit flag
                m_packetQueue.shutdown();
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
            
            // 重置时间戳平滑器
            m_timestampSmoother.reset();

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
            m_packetQueue.push(pkt1);
            return 0;
        }
        
        /**
         * 配置时间戳平滑参数
         * @param smoothingFactor 平滑系数 (0.01-1.0)，越小越平滑
         * @param maxJumpThreshold 最大允许跳跃阈值
         * @param minIncrement 最小时间戳增量
         */
        void configureTimestampSmoother(double smoothingFactor = 0.1, 
                                       int64_t maxJumpThreshold = 90000, 
                                       int64_t minIncrement = 3000) {
            m_timestampSmoother.setSmoothingParameters(smoothingFactor, maxJumpThreshold, minIncrement);
        }
        
        /**
         * 为不同场景预设时间戳平滑参数
         */
        void setTimestampSmoothingPreset(const std::string& preset) {
            if (preset == "conservative") {
                // 保守模式：较少干预，适合时间戳相对准确的流
                m_timestampSmoother.setSmoothingParameters(0.05, 180000, 1000);
            } else if (preset == "aggressive") {
                // 激进模式：强力平滑，适合时间戳很不准确的流
                m_timestampSmoother.setSmoothingParameters(0.3, 30000, 3000);
            } else if (preset == "looping") {
                // 回环模式：专门处理文件回环的情况
                m_timestampSmoother.setSmoothingParameters(0.1, 45000, 2000);
            } else {
                // 默认模式：平衡的设置
                m_timestampSmoother.setSmoothingParameters(0.1, 90000, 3000);
            }
            
            std::cout << "Timestamp smoothing preset set to: " << preset << std::endl;
        }
        
        /**
         * 获取时间戳平滑统计信息
         */
        void getTimestampStatistics(int64_t& totalPackets, int64_t& correctedPackets, double& correctionRate) const {
            m_timestampSmoother.getStatistics(totalPackets, correctedPackets, correctionRate);
        }
        
        int closeOutputStream() {
            std::cout << "call CloseOutputStream()" << std::endl;
            
            // 打印时间戳平滑统计信息
            m_timestampSmoother.printStatistics();
            
            m_repeat = false;
            m_outputState = Down;
            
            // Signal shutdown to unblock any waiting operations
            m_packetQueue.shutdown();
            
            if (m_threadOutput) {
                m_threadOutput->join();
                delete m_threadOutput;
                m_threadOutput = nullptr;
            }

            // Reset the queue for potential reuse
            m_packetQueue.reset();

            if (m_ofmtCtx) {
                avformat_free_context(m_ofmtCtx);
                m_ofmtCtx = NULL;
            }

            return 0;
        }
    };
} // namespace otl

#endif // STREAM_PUSHER_H
