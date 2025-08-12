//
// Created by yuan on 25-8-12.
//

#ifndef STREAM_ENCODER_H
#define STREAM_ENCODER_H

#include "otl_ffmpeg.h"
#include "otl_log.h"
#include <memory>
#include <string>
#include <atomic>
#include <cstdint>

namespace otl {
    // 编码参数（便捷设置常用项，可通过options扩展）
    struct EncodeParam {
        std::string codecName{"h264"};
        int width{0};
        int height{0};
        AVRational timeBase{1, 90000};
        AVRational frameRate{0, 1};    // 可选，0 表示未知
        AVPixelFormat pixFmt{AV_PIX_FMT_YUV420P};
        int64_t bitRate{0};            // bps，0 表示按 crf/qp 控制
        int gopSize{0};                // 0 由编码器决定
        int maxBFrames{-1};            // <0 使用编码器默认
        int threadCount{0};            // 0 自动

        // 码控相关
        int crf{-1};                   // libx264/libx265 等支持
        int qp{-1};                    // 固定量化参数
        std::string preset;            // veryfast, medium, slow ...
        std::string tune;              // zerolatency, film ...
        std::string profile;           // high, main, baseline ...

        // 额外的键值对，直接透传到 priv_data
        AVDictionary* options{nullptr}; // 不接管生命周期

        // 硬件编码偏好
        bool preferHardware{true};       // 优先尝试硬件编码（如可用）
        std::string hwAccel;             // 指定首选后端：videotoolbox/nvenc/qsv/vaapi/amf（留空则按平台优先级）
    };

    class StreamEncoder {
    public:
        StreamEncoder();
        virtual ~StreamEncoder();
        virtual int init(EncodeParam *params) = 0;
        virtual int encode(AVFrame* frame, AVPacket **p_pkt, int *p_num) = 0;

        // 请求下一个帧编码为关键帧（IDR）
        virtual int requestKeyFrame() = 0;

        // 获取编码帧率统计
        // 返回：平均 fps；frames 返回累计帧数；elapsedSec 返回起始到当前耗时（秒）
        virtual double getFps(uint64_t &frames, double &elapsedSec) const = 0;
    };

    // 根据编码名称创建编码器（h264/h265/hevc/mjpeg/mpeg4/libx264/libx265/...）
    std::unique_ptr<StreamEncoder> CreateStreamEncoder(const std::string &codecName);
}




#endif //STREAM_ENCODER_H
