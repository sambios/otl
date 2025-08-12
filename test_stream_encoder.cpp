#include "stream_encoder.h"
#include "otl_log.h"
#include <vector>
#include <string>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace otl;

static AVFrame* make_test_frame(int w, int h, AVPixelFormat fmt, int64_t pts) {
    AVFrame* f = av_frame_alloc();
    f->width = w;
    f->height = h;
    f->format = fmt;
    if (av_frame_get_buffer(f, 32) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    if (av_frame_make_writable(f) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    // Fill YUV420P with gray bars
    if (fmt == AV_PIX_FMT_YUV420P) {
        int Y = 0x80, U = 0x80, V = 0x80;
        for (int y = 0; y < h; ++y) {
            memset(f->data[0] + y * f->linesize[0], Y, w);
        }
        for (int y = 0; y < h/2; ++y) {
            memset(f->data[1] + y * f->linesize[1], U, w/2);
            memset(f->data[2] + y * f->linesize[2], V, w/2);
        }
    }
    f->pts = pts;
    return f;
}

static int encode_frames(StreamEncoder& enc, int frames, int w, int h, AVPixelFormat fmt, AVRational tb, int startPts, int step) {
    int totalPkts = 0;
    for (int i = 0; i < frames; ++i) {
        AVFrame* f = make_test_frame(w, h, fmt, startPts + i*step);
        if (!f) return -1;
        AVPacket* pkt = nullptr; int num = 0;
        int ret = enc.encode(f, &pkt, &num);
        av_frame_free(&f);
        if (ret < 0) return ret;
        if (num == 1 && pkt) {
            ++totalPkts;
            av_packet_free(&pkt);
        }
    }
    // try flush
    for (int i = 0; i < 4; ++i) {
        AVPacket* pkt = nullptr; int num = 0;
        if (enc.encode(nullptr, &pkt, &num) < 0) break;
        if (num == 1 && pkt) { ++totalPkts; av_packet_free(&pkt); }
    }
    return totalPkts;
}

static bool test_functional(const std::string& codec)
{
    OTL_LOGI("TEST", "Functional test codec=%s", codec.c_str());
    auto enc = CreateStreamEncoder(codec);
    EncodeParam p;
    p.codecName = codec;
    p.width = 320; p.height = 240;
    p.timeBase = {1, 90000};
    p.frameRate = {30, 1};
    p.pixFmt = AV_PIX_FMT_YUV420P;
    p.gopSize = 30; p.maxBFrames = 0;
    p.preferHardware = false; // tests use SW path for stability across environments

    if (enc->init(&p) < 0) {
        OTL_LOGW("TEST", "init failed for codec=%s (may be unavailable)", codec.c_str());
        return true; // don't fail entire suite due to missing encoder in this build
    }

    // Request IDR before first frame
    enc->requestKeyFrame();

    int pkts = encode_frames(*enc, 10, p.width, p.height, p.pixFmt, p.timeBase, 0, 3000);
    if (pkts <= 0) {
        OTL_LOGE("TEST", "no packets produced for codec=%s", codec.c_str());
        return false;
    }

    uint64_t frames = 0; double sec = 0.0; double fps = enc->getFps(frames, sec);
    OTL_LOGI("TEST", "fps=%.2f frames=%llu sec=%.2f", fps, (unsigned long long)frames, sec);
    return frames > 0;
}

static bool test_performance(const std::string& codec)
{
    OTL_LOGI("TEST", "Performance test codec=%s", codec.c_str());
    auto enc = CreateStreamEncoder(codec);
    EncodeParam p;
    p.codecName = codec;
    p.width = 1280; p.height = 720;
    p.timeBase = {1, 90000};
    p.frameRate = {30, 1};
    p.pixFmt = AV_PIX_FMT_YUV420P;
    p.gopSize = 60; p.maxBFrames = 0;
    p.bitRate = 3'000'000;
    p.preferHardware = false; // stabilize perf test across CI/macOS environments

    if (enc->init(&p) < 0) {
        OTL_LOGW("TEST", "perf init failed for codec=%s (skip)", codec.c_str());
        return true; // skip when unavailable
    }

    const int N = 60;
    auto t0 = std::chrono::steady_clock::now();
    int pkts = encode_frames(*enc, N, p.width, p.height, p.pixFmt, p.timeBase, 0, 3000);
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    OTL_LOGI("TEST", "perf: frames=%d packets=%d time=%.3fs fps=%.2f", N, pkts, sec, (double)N/sec);
    return pkts > 0;
}

static bool test_exceptions()
{
    OTL_LOGI("TEST", "Exception tests");

    // 1) init with nullptr params
    {
        auto enc = CreateStreamEncoder("h264");
        int ret = enc->init(nullptr);
        if (ret >= 0) { OTL_LOGE("TEST", "init(nullptr) should fail"); return false; }
    }

    // 2) invalid size
    {
        auto enc = CreateStreamEncoder("h264");
        EncodeParam p; p.codecName = "h264"; p.width = 0; p.height = 0; p.pixFmt = AV_PIX_FMT_YUV420P; p.preferHardware = false;
        int ret = enc->init(&p);
        if (ret >= 0) { OTL_LOGE("TEST", "init with invalid size should fail"); return false; }
    }

    // 3) unknown codec
    {
        auto enc = CreateStreamEncoder("this_codec_does_not_exist");
        EncodeParam p; p.codecName = "this_codec_does_not_exist"; p.width = 320; p.height = 240; p.pixFmt = AV_PIX_FMT_YUV420P; p.preferHardware = false;
        int ret = enc->init(&p);
        if (ret >= 0) { OTL_LOGE("TEST", "init should fail for unknown codec"); return false; }
    }

    // 4) encode before init should fail
    {
        auto enc = CreateStreamEncoder("h264");
        AVPacket* pkt = nullptr; int num = 0;
        int ret = enc->encode(nullptr, &pkt, &num);
        if (ret >= 0) { OTL_LOGE("TEST", "encode before init should fail"); return false; }
    }

    return true;
}

int main()
{
    OTL_INIT_LOG("stream_encoder_test");

    std::vector<std::string> codecs = {"h264", "hevc", "mjpeg"};

    bool ok = true;
    for (auto &c : codecs) ok = test_functional(c) && ok;
    for (auto &c : codecs) ok = test_performance(c) && ok;
    ok = test_exceptions() && ok;

    if (!ok) {
        OTL_LOGE("TEST", "stream_encoder tests FAILED");
        return 2;
    }
    OTL_LOGI("TEST", "stream_encoder tests PASSED");
    // Avoid issues with static destructors in some FFmpeg/codec builds
    _exit(0);
}
