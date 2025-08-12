#include "stream_encoder.h"

#include <chrono>
#include <vector>

namespace otl {

static const char* TAG = "StreamEncoder";

class FfmpegStreamEncoder final : public StreamEncoder {
public:
    FfmpegStreamEncoder(const std::string& codecName)
        : mCodecName(codecName) {
        mStart = std::chrono::steady_clock::now();
        mFrameCount.store(0);
        mForceIdr.store(false);
    }

    ~FfmpegStreamEncoder() override {
        close();
    }

    int init(EncodeParam* params) override {
        if (!params) return AVERROR(EINVAL);
        mParams = *params;

        const AVCodec* codec = resolveCodec(mParams.codecName);
        if (!codec) {
            OTL_LOGE(TAG, "codec not found for name=%s", mParams.codecName.c_str());
            return AVERROR_ENCODER_NOT_FOUND;
        }

        mCtx = avcodec_alloc_context3(codec);
        if (!mCtx) return AVERROR(ENOMEM);

        mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
        mCtx->codec_id   = codec->id;
        mCtx->width      = mParams.width;
        mCtx->height     = mParams.height;
        mCtx->time_base  = mParams.timeBase.num > 0 ? mParams.timeBase : (AVRational{1, 90000});
        if (mParams.frameRate.num > 0 && mParams.frameRate.den > 0) {
            mCtx->framerate = mParams.frameRate;
        }
        mCtx->pix_fmt    = mParams.pixFmt;
        if (mParams.bitRate > 0) mCtx->bit_rate = mParams.bitRate;
        if (mParams.gopSize > 0) mCtx->gop_size = mParams.gopSize;
        if (mParams.maxBFrames >= 0) mCtx->max_b_frames = mParams.maxBFrames;
        if (mParams.threadCount > 0) mCtx->thread_count = mParams.threadCount;

        // Common low-latency hints
#ifdef AV_CODEC_FLAG_GLOBAL_HEADER
        // leave global header decision to muxer; do not enforce here
#endif

        AVDictionary* opts = nullptr;
        if (mParams.options) {
            // clone external options but do not take ownership
            av_dict_copy(&opts, mParams.options, 0);
        }

        // Preset/tune/profile
        if (!mParams.preset.empty())   av_dict_set(&opts, "preset",  mParams.preset.c_str(), 0);
        if (!mParams.tune.empty())     av_dict_set(&opts, "tune",    mParams.tune.c_str(), 0);
        if (!mParams.profile.empty())  av_dict_set(&opts, "profile", mParams.profile.c_str(), 0);
        if (mParams.crf >= 0) {
            // CRF is widely supported by libx264/libx265
            char buf[32]; snprintf(buf, sizeof(buf), "%d", mParams.crf);
            av_dict_set(&opts, "crf", buf, 0);
        }
        if (mParams.qp >= 0) {
            // Some encoders support constant QP via q or qp
            char buf[32]; snprintf(buf, sizeof(buf), "%d", mParams.qp);
            av_dict_set(&opts, "qp", buf, 0);
            av_dict_set(&opts, "q", buf, 0);
        }

        int ret = avcodec_open2(mCtx, codec, &opts);
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err, sizeof(err));
            OTL_LOGE(TAG, "avcodec_open2 failed: %s", err);
            avcodec_free_context(&mCtx);
            av_dict_free(&opts);
            return ret;
        }
        av_dict_free(&opts);

        OTL_LOGI(TAG, "encoder opened: %s %dx%d pixfmt=%d br=%lld gop=%d b=%d tb=%d/%d fr=%d/%d",
                 mParams.codecName.c_str(), mCtx->width, mCtx->height, mCtx->pix_fmt,
                 (long long)mCtx->bit_rate, mCtx->gop_size, mCtx->max_b_frames,
                 mCtx->time_base.num, mCtx->time_base.den,
                 mCtx->framerate.num, mCtx->framerate.den);
        mStart = std::chrono::steady_clock::now();
        mFrameCount.store(0);
        return 0;
    }

    int encode(AVFrame* frame, AVPacket** p_pkt, int* p_num) override {
        if (!mCtx) return AVERROR(EINVAL);
        if (!p_pkt || !p_num) return AVERROR(EINVAL);
        *p_pkt = nullptr; *p_num = 0;

        // apply on-demand keyframe request
        if (frame && mForceIdr.exchange(false)) {
            frame->pict_type = AV_PICTURE_TYPE_I;
            frame->key_frame = 1;
        }

        int ret = avcodec_send_frame(mCtx, frame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            logAvError("avcodec_send_frame", ret);
            return ret;
        }

        // Try receive one packet (API allows many; we return the first for simplicity)
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return AVERROR(ENOMEM);
        ret = avcodec_receive_packet(mCtx, pkt);
        if (ret == 0) {
            *p_pkt = pkt;
            *p_num = 1;
            mFrameCount.fetch_add(1, std::memory_order_relaxed);
            return 0;
        } else {
            av_packet_free(&pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0; // no output yet
            logAvError("avcodec_receive_packet", ret);
            return ret;
        }
    }

    int requestKeyFrame() override {
        mForceIdr.store(true, std::memory_order_relaxed);
        return 0;
    }

    double getFps(uint64_t &frames, double &elapsedSec) const override {
        frames = mFrameCount.load(std::memory_order_relaxed);
        auto now = std::chrono::steady_clock::now();
        elapsedSec = std::chrono::duration<double>(now - mStart).count();
        if (elapsedSec <= 0.0) return 0.0;
        return static_cast<double>(frames) / elapsedSec;
    }

private:
    const AVCodec* resolveCodec(const std::string& name) {
        std::string lower = name;
        for (auto &c : lower) c = (char)tolower(c);
        // Normalize some aliases
        if (lower == "h265") lower = "hevc";
        if (lower == "x264") lower = "h264";   // treat as family, choose hw if available
        if (lower == "x265") lower = "hevc";

        // Build candidate list, prefer hardware if requested
        std::vector<std::string> candidates;

        auto push_if = [&](const std::string& v) {
            if (!v.empty()) candidates.push_back(v);
        };

        auto push_hw_family = [&](const std::string& family){
            // Respect explicit hwAccel preference first
            if (!mParams.hwAccel.empty()) {
                std::string acc = mParams.hwAccel;
                for (auto &ch : acc) ch = (char)tolower(ch);
                if (family == "h264") {
                    if (acc == "videotoolbox") push_if("h264_videotoolbox");
                    if (acc == "nvenc" || acc == "cuda") push_if("h264_nvenc");
                    if (acc == "qsv") push_if("h264_qsv");
                    if (acc == "amf") push_if("h264_amf");
                    if (acc == "vaapi") push_if("h264_vaapi");
                } else if (family == "hevc") {
                    if (acc == "videotoolbox") push_if("hevc_videotoolbox");
                    if (acc == "nvenc" || acc == "cuda") push_if("hevc_nvenc");
                    if (acc == "qsv") push_if("hevc_qsv");
                    if (acc == "amf") push_if("hevc_amf");
                    if (acc == "vaapi") push_if("hevc_vaapi");
                }
            } else {
                // Platform default preference order
#if defined(__APPLE__)
                if (family == "h264") push_if("h264_videotoolbox");
                if (family == "hevc") push_if("hevc_videotoolbox");
#endif
                if (family == "h264") {
                    push_if("h264_nvenc");
                    push_if("h264_qsv");
                    push_if("h264_amf");
                    push_if("h264_vaapi");
                } else if (family == "hevc") {
                    push_if("hevc_nvenc");
                    push_if("hevc_qsv");
                    push_if("hevc_amf");
                    push_if("hevc_vaapi");
                }
            }
        };

        if (mParams.preferHardware) {
            if (lower == "h264") push_hw_family("h264");
            if (lower == "hevc") push_hw_family("hevc");
        }

        // Then try the exact name as given (SW or HW)
        push_if(lower);

        // Finally, software fallbacks by common names
        if (lower == "h264") push_if("libx264");
        if (lower == "hevc") push_if("libx265");
        if (lower == "mjpeg") push_if("mjpeg");
        if (lower == "mpeg4") push_if("mpeg4");

        // Probe in order
        for (const auto& n : candidates) {
            if (const AVCodec* c = avcodec_find_encoder_by_name(n.c_str())) {
                return c;
            }
        }

        // Last resort: by ID
        if (lower == "h264" || lower == "libx264") return avcodec_find_encoder(AV_CODEC_ID_H264);
        if (lower == "hevc" || lower == "h265" || lower == "libx265") return avcodec_find_encoder(AV_CODEC_ID_HEVC);
        if (lower == "mjpeg" || lower == "jpeg") return avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (lower == "mpeg4") return avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        return nullptr;
    }

    void close() {
        if (mCtx) {
            avcodec_free_context(&mCtx);
            mCtx = nullptr;
        }
    }

    static void logAvError(const char* what, int err) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, buf, sizeof(buf));
        OTL_LOGE(TAG, "%s failed: %s", what, buf);
    }

private:
    std::string mCodecName;
    EncodeParam mParams{};
    AVCodecContext* mCtx{nullptr};

    std::atomic<bool> mForceIdr{false};
    std::atomic<uint64_t> mFrameCount{0};
    std::chrono::steady_clock::time_point mStart;
};

std::unique_ptr<StreamEncoder> CreateStreamEncoder(const std::string &codecName) {
    return std::unique_ptr<StreamEncoder>(new FfmpegStreamEncoder(codecName));
}

// Default base ctor/dtor definitions
StreamEncoder::StreamEncoder() = default;
StreamEncoder::~StreamEncoder() = default;

} // namespace otl
