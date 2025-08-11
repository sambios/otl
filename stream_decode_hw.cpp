#include "stream_decode_hw.h"
#include "otl.h"
#include "stream_sei.h"

extern "C" {
 #include <libavfilter/avfilter.h>
 #include <libavfilter/buffersrc.h>
 #include <libavfilter/buffersink.h>
 #include <libavutil/opt.h>
 #include <libavutil/buffer.h>
}
#include <unistd.h>

namespace otl
{

void print_ffmpeg_error(int err_code)
{
    char err_buf[1024];

    av_strerror(err_code, err_buf, sizeof(err_buf));

    printf("FFmpeg ERROR: %s (errcode: %d)\n", err_buf, err_code);
}

// 硬件加速格式选择回调函数
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    StreamDecoder *decoder = static_cast<StreamDecoder *>(ctx->opaque);
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++)
    {
        if (*p == decoder->mHwPixFmt)
            return *p;
    }

    // 记录日志但返回第一个可用格式作为回退
    av_log(ctx, AV_LOG_WARNING, "Failed to get preferred HW surface format, trying alternatives.\n");
    return pix_fmts[0]; // 回退到解码器提供的第一个格式
}

StreamDecoder::StreamDecoder(int id, AVCodecContext *decoder) : mObserver(nullptr), mExternalDecCtx(decoder)
{
    std::cout << "StreamDecoder() ctor..." << std::endl;
    mIsWaitingIframe = true;
    mId = id;
    mOptsDecoder = nullptr;
    // device type
    // 只需要设置DeviceName, 程序自动设置mHwPixFmt值，避免编译问题。
    strcpy(mszHWDevTypeName, "cuda");
    if (strcmp(mszHWDevTypeName, "vsv") == 0) // VSV HWAccel
    {
        strcpy(mszHWDevTypeName, "vsv");
        strcpy(mszHWDecoderName, "h264_vsv_decoder");

        //1. first check /opt/tops/lib/lib_vcd.so
        char* envVcdSo = getenv("VCD_SHARED_LIB");
        if (envVcdSo == nullptr) {
            std::string vcd_lib_dir = "/opt/tops/lib/lib_vcd.so";
            if (access(vcd_lib_dir.c_str(), F_OK) == 0) {
                setenv("VCD_SHARED_LIB", vcd_lib_dir.c_str(), 1);
            }else {
                std::string homedir= getenv("HOME");
                vcd_lib_dir = homedir + "/enrigin_sdk/opt/tops/lib/lib_vcd.so";
                setenv("VCD_SHARED_LIB", vcd_lib_dir.c_str(), 1);
            }
        }
    }
    else if (strcmp(mszHWDevTypeName, "cuda") == 0) // CUDA HWAccel
    {
        strcpy(mszHWDecoderName, "h264_cuvid");
    }
}

StreamDecoder::~StreamDecoder()
{
    std::cout << "~StreamDecoder() dtor..." << std::endl;

    // release filter graph resources
    if (mFilterGraph) {
        avfilter_graph_free(&mFilterGraph);
        mFilterGraph = nullptr;
        mBuffersrcCtx = nullptr;
        mBuffersinkCtx = nullptr;
        mFilterInited = false;
        mEnableFilter = false;
        mFilterDesc.clear();
    }

    // 释放硬件设备上下文
    if (mHWDeviceCtx)
    {
        av_buffer_unref(&mHWDeviceCtx);
        mHWDeviceCtx = nullptr;
    }

    av_dict_free(&mOptsDecoder);
}

int StreamDecoder::decodeFrame(AVPacket *pkt, AVFrame *pFrame)
{
    AVCodecContext *decCtx = nullptr;
    if (nullptr == mExternalDecCtx)
    {
        decCtx = mDecCtx;
    }
    else
    {
        decCtx = mExternalDecCtx;
    }

    int gotPicture = 0;

#if LIBAVCODEC_VERSION_MAJOR > 56
    // 发送数据包到解码器
    int ret = avcodec_send_packet(decCtx, pkt);
    if (ret < 0)
    {
        if (ret == AVERROR(EAGAIN))
        {
            // 解码器已满，先尝试取帧再重试
            int receive_ret;
            do
            {
                receive_ret = avcodec_receive_frame(decCtx, pFrame);
                if (receive_ret >= 0)
                {
                    // 成功接收到一帧
                    gotPicture++;
                    // 不在此处下载硬件帧，尽量保持硬件加速
                }
            } while (receive_ret >= 0);

            // 接收完所有可用帧后，重新尝试发送当前包
            ret = avcodec_send_packet(decCtx, pkt);
            if (ret < 0)
            {
                printf("Error sending packet after receiving frames: %d\n", ret);
                print_ffmpeg_error(ret);
                return ret; // 返回错误码
            }
        }
        else
        {
            // 其他错误
            printf("Error sending packet for decoding, decCtx = %p\n", decCtx);
            print_ffmpeg_error(ret);
            return ret; // 返回错误码
        }
    }

    // 循环从解码器接收帧（一个数据包可能解码出多帧）
    while (true)
    {
        ret = avcodec_receive_frame(decCtx, pFrame);
        if (ret == AVERROR(EAGAIN))
        {
            // 需要更多数据包
            break;
        }
        else if (ret == AVERROR_EOF)
        {
            printf("avcodec_receive_frame() err=end of file!\n");
            break;
        }
        else if (ret < 0)
        {
            print_ffmpeg_error(ret);
            return ret; // 返回错误码
        }

        gotPicture++;

        // 如果是硬件帧，需要将其从硬件设备内存转移到系统内存
        // 不再在 decode 阶段将硬件帧下载为系统内存帧，保留 HW 帧以尽量保持硬件加速。

        // 如果调用者期望只处理一帧，此处应该可以返回
        // 但为了支持一次解码多帧，我们继续处理直到需要更多数据
        break;
    }

    return gotPicture;

#else
    int gotFrame = 0;
    int ret = avcodec_decode_video2(decCtx, pFrame, &gotFrame, pkt);
    if (ret < 0)
    {
        return -1;
    }

    if (gotFrame > 0)
    {
        return 1;
    }

    return 0;
#endif
}

void StreamDecoder::onAvformatOpened(AVFormatContext *ifmtCtx)
{
    if (mOnAvformatOpenedFunc != nullptr)
    {
        mOnAvformatOpenedFunc(ifmtCtx);
    }

    if (mExternalDecCtx == nullptr)
    {
        if (0 == createVideoDecoder(ifmtCtx))
        {
            printf("create video decoder ok!\n");
        }
    }

    if (strcmp(ifmtCtx->iformat->name, "h264") != 0)
    {
        mIsWaitingIframe = false;
    }
}

void StreamDecoder::onAvformatClosed()
{
    clearPackets();
    std::cout << __FUNCTION__ << ":" << __LINE__ << std::endl;
    // free filter graph
    if (mFilterGraph) {
        avfilter_graph_free(&mFilterGraph);
        mFilterGraph = nullptr;
        mBuffersrcCtx = nullptr;
        mBuffersinkCtx = nullptr;
        mFilterInited = false;
    }
    // 释放硬件设备上下文
    if (mHWDeviceCtx)
    {
        av_buffer_unref(&mHWDeviceCtx);
        mHWDeviceCtx = nullptr;
    }

    if (mDecCtx != nullptr)
    {
        avcodec_close(mDecCtx);
        avcodec_free_context(&mDecCtx);
        printf("free video decoder context!\n");
    }

    if (mOnAvformatClosedFunc)
        mOnAvformatClosedFunc();
}

int StreamDecoder::onReadFrame(AVPacket *pkt)
{
    int ret = 0;

    if (mVideoStreamIndex != pkt->stream_index)
    {
        // ignore other streams if not video.
        return 0;
    }

    if (mIsWaitingIframe)
    {
        if (isKeyFrame(pkt))
        {
            mIsWaitingIframe = false;
        }
    }

    if (mIsWaitingIframe)
    {
        return 0;
    }

    auto decCtx = mExternalDecCtx != nullptr ? mExternalDecCtx : mDecCtx;

    if (decCtx->codec_id == AV_CODEC_ID_H264)
    {
        std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
        int seiLen = 0;
        // Always try to read SEI; h264SeiPacketRead supports Annex B and AVCC
        seiLen = h264SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);

        if (seiLen > 0)
        {
            if (mObserver != nullptr) {
                mObserver->onDecodedSeiInfo(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
            }

            if (mOnDecodedSeiFunc) {
                mOnDecodedSeiFunc(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
            }
        }
    }
    else if (decCtx->codec_id == AV_CODEC_ID_H265)
    {
        std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
        int seiLen = 0;
        // Always try to read SEI; h265SeiPacketRead supports Annex B and AVCC
        seiLen = h265SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
        if (seiLen > 0)
        {
            if (mObserver != nullptr)
            {
                mObserver->onDecodedSeiInfo(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
            }

            if (mOnDecodedSeiFunc != nullptr)
            {
                mOnDecodedSeiFunc(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
            }
        }
    }

    //std::cout << __FUNCTION__ << ":" << __LINE__ << std::endl;
    AVFrame *frame = av_frame_alloc();
    ret = decodeFrame(pkt, frame);
    if (ret < 0)
    {
        printf("decode failed!\n");
        av_frame_free(&frame);
        return ret;
    }

    if (mFrameDecodedNum == 0)
    {
        printf("id=%d, ffmpeg delayed frames: %d\n", mId, (int)mListPackets.size());
    }

    if (ret > 0)
        mFrameDecodedNum++;

    putPacket(pkt);

    if (ret > 0)
    {
        auto pktS = getPacket();
        AVFrame *outFrame = frame;

        // Lazy init filter graph when first frame arrives
        if (mEnableFilter && !mFilterInited) {
            if (initFilterGraphWithFrame(mExternalDecCtx != nullptr ? mExternalDecCtx : mDecCtx, frame) == 0) {
                mFilterInited = true;
            } else {
                fprintf(stderr, "initFilterGraphWithFrame failed, disable filtering.\n");
                mEnableFilter = false;
            }
        }

        // Apply filters if enabled
        AVFrame *filtered = nullptr;
        if (mEnableFilter && mFilterInited) {
            filtered = av_frame_alloc();
            int fr = applyFilters(frame, filtered);
            if (fr == 0) {
                outFrame = filtered;
            } else {
                av_frame_free(&filtered);
            }
        }

        if (mObserver)
        {
            mObserver->onDecodedAVFrame(pktS, outFrame);
        }

        if (mOnDecodedFrameFunc != nullptr)
        {
            mOnDecodedFrameFunc(pktS, outFrame);
        }

        av_packet_unref(pktS);
        av_freep(&pktS);

        if (outFrame != frame) {
            av_frame_free(&outFrame);
        }
    }

    av_frame_unref(frame);
    av_frame_free(&frame);

    return ret;
}

void StreamDecoder::onReadEof(AVPacket *pkt)
{
    std::cout << __FUNCTION__ << ":" << __LINE__ << std::endl;
    mFrameDecodedNum = 0;
    clearPackets();

    if (mObserver)
    {
        mObserver->onStreamEof();
    }

    if (mOnReadEofFunc != nullptr)
    {
        mOnReadEofFunc(nullptr);
    }
}

int StreamDecoder::putPacket(AVPacket *pkt)
{
#if LIBAVCODEC_VERSION_MAJOR > 56
    AVPacket *pktNew = av_packet_alloc();
#else
    AVPacket *pktNew = (AVPacket *)av_malloc(sizeof(AVPacket));
    av_init_packet(pktNew);
#endif

    av_packet_ref(pktNew, pkt);
    mListPackets.push_back(pktNew);
    return 0;
}

AVPacket *StreamDecoder::getPacket()
{
    if (mListPackets.size() == 0)
        return nullptr;
    auto pkt = mListPackets.front();
    mListPackets.pop_front();
    return pkt;
}

void StreamDecoder::clearPackets()
{
    while (mListPackets.size() > 0)
    {
        auto pkt = mListPackets.front();
        mListPackets.pop_front();
#if LIBAVCODEC_VERSION_MAJOR > 56
        av_packet_free(&pkt);
#else
        av_free_packet(pkt);
        av_free(pkt);
#endif
    }
}

int StreamDecoder::getVideoStreamIndex(AVFormatContext *ifmtCtx)
{
    for (unsigned int i = 0; i < ifmtCtx->nb_streams; i++)
    {
#if LIBAVFORMAT_VERSION_MAJOR > 56
        auto codecType = ifmtCtx->streams[i]->codecpar->codec_type;
#else
        auto codecType = ifmtCtx->streams[i]->codec->codec_type;
#endif
        if (codecType == AVMEDIA_TYPE_VIDEO)
        {
            mVideoStreamIndex = i;
            break;
        }
    }
    return mVideoStreamIndex;
}

AVCodecID StreamDecoder::getVideoCodecId()
{
    if (mDecCtx)
    {
        return mDecCtx->codec_id;
    }
    return AV_CODEC_ID_NONE;
}

int ret = 0;

int StreamDecoder::initHWConfig(int devId, int vpuId)
{
    mHWDevType = av_hwdevice_find_type_by_name(mszHWDevTypeName);
    if (mHWDevType == AV_HWDEVICE_TYPE_NONE)
    {
        fprintf(stderr, "Device type %s is not supported.\n", mszHWDevTypeName);
        fprintf(stderr, "Available device types:");
        while ((mHWDevType = av_hwdevice_iterate_types(mHWDevType)) != AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(mHWDevType));
        fprintf(stderr, "\n");
        return -1;
    }

    AVDictionary *opts = nullptr;

    char tmp[128];
    sprintf(tmp, "/dev/gcu%dvid%d", devId, vpuId);
    printf("create decoderID = %d, hwdevicectx %s\n", mId, tmp);
    av_dict_set(&opts, "dec", tmp, 0);
    av_dict_set(&opts, "enc", tmp, 0);
    sprintf(tmp, "/dev/gcu%d", devId);
    av_dict_set(&opts, "mem", tmp, 0);
    av_dict_set(&opts, "mapped_io", "1", 0);

    if (av_hwdevice_ctx_create(&mHWDeviceCtx, mHWDevType, nullptr, opts, 0) < 0)
    {
        fprintf(stderr, "Hardware device context creation failed\n");
        av_dict_free(&opts);
        return -1;
    }

    av_dict_free(&opts);
    return 0;
}

int StreamDecoder::createVideoDecoder(AVFormatContext *ifmtCtx)
{
    int videoIndex = getVideoStreamIndex(ifmtCtx);
    mTimebase = ifmtCtx->streams[videoIndex]->time_base;
    bool isHWConfigOK = false;
    // 尝试初始化硬件配置
    int card_id = OTL_GET_INT32_HIGH16(mId);
    int vid = OTL_GET_INT32_LOW16(mId);
    if (initHWConfig(card_id, vid) < 0)
    {
        fprintf(stderr, "Hardware acceleration initialization failed.\n");
        isHWConfigOK = false;
    }else
    {
        isHWConfigOK = true;
    }

#if LIBAVCODEC_VERSION_MAJOR > 56
    auto codecId = ifmtCtx->streams[videoIndex]->codecpar->codec_id;
#else
    auto codecId = ifmtCtx->streams[videoIndex]->codec->codec_id;
#endif

    const AVCodec *codec = nullptr;
    if (isHWConfigOK)
    {
        switch (codecId)
        {
        case AV_CODEC_ID_H264:
            codec = avcodec_find_decoder_by_name(mszHWDecoderName);
            break;
        case AV_CODEC_ID_HEVC:
            codec = avcodec_find_decoder_by_name("hevc_vsv_decoder");
            break;
        case AV_CODEC_ID_VP9:
            codec = avcodec_find_decoder_by_name("vp9_vsv_decoder");
            break;
        case AV_CODEC_ID_MJPEG:
            codec = avcodec_find_decoder_by_name("jpeg_vsv_decoder");
            break;
        default:
            break;
        }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 18, 100)
        for (int i = 0;; i++)
        {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
            if (!config)
            {
                fprintf(stderr, "Decoder %s does not support device type %s.\n", codec->name,
                        av_hwdevice_get_type_name(mHWDevType));
                return -1;
            }

            fprintf(stderr, "Decoder: %s, device type: %s.\n", codec->name, av_hwdevice_get_type_name(mHWDevType));

            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == mHWDevType)
            {
                mHwPixFmt = config->pix_fmt;
                fprintf(stderr, "hw_pix_fmt %d\n", mHwPixFmt);
                break;
            }
        }
#endif
    }

    if (nullptr == codec)
    {
        codec = avcodec_find_decoder(codecId);
        if (!codec)
        {
            printf("can't find code_id %d\n", codecId);
            return -1;
        }
    }

    if (mDecCtx != nullptr)
    {
        avcodec_close(mDecCtx);
        avcodec_free_context(&mDecCtx);
        mDecCtx = nullptr;
    }

    mDecCtx = avcodec_alloc_context3(codec);
    if (mDecCtx == NULL)
    {
        printf("avcodec_alloc_context3 err");
        return -1;
    }

    int ret = 0;

#if LIBAVCODEC_VERSION_MAJOR > 56
    if ((ret = avcodec_parameters_to_context(mDecCtx, ifmtCtx->streams[videoIndex]->codecpar)) < 0)
    {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }
#else
    if ((ret = avcodec_copy_context(mDecCtx, ifmtCtx->streams[videoIndex]->codec)) < 0)
    {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }

    if (codec->capabilities & AV_CODEC_CAP_TRUNCATED)
    {
        mDecCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
    }
#endif


    if (isHWConfigOK) {
        av_dict_set_int(&mOptsDecoder, "card_id", card_id, 0);
        av_dict_set_int(&mOptsDecoder, "vpu_id", vid, 0);
        // 不再强制输出像素格式为 yuv420p，允许保持硬件表面

        mDecCtx->hw_device_ctx = av_buffer_ref(mHWDeviceCtx);
        mDecCtx->opaque = this;
        mDecCtx->get_format = get_hw_format;
        // 不设置 mDecCtx->pix_fmt 为 SW 格式，交由解码器选择 HW 表面格式
    }

    if ((ret = avcodec_open2(mDecCtx, codec, &mOptsDecoder)) < 0)
    {
        fprintf(stderr, "Failed to open %s codec\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }

    return 0;
}

int StreamDecoder::setObserver(StreamDecoderEvents *observer)
{
    mObserver = observer;
    return 0;
}

int StreamDecoder::openStream(std::string url, bool repeat, AVDictionary *opts)
{
    av_dict_copy(&mOptsDecoder, opts, 0);
    // parse filter string from opts without changing external interface
    AVDictionaryEntry *e = av_dict_get(mOptsDecoder, "filter", nullptr, 0);
    if (!e) e = av_dict_get(mOptsDecoder, "vf", nullptr, 0);
    if (e && e->value && e->value[0] != '\0') {
        mFilterDesc = e->value;
        mEnableFilter = true;
        // reset any previous graph
        if (mFilterGraph) {
            avfilter_graph_free(&mFilterGraph);
            mFilterGraph = nullptr;
            mBuffersrcCtx = nullptr;
            mBuffersinkCtx = nullptr;
        }
        mFilterInited = false;
    } else {
        mFilterDesc.clear();
        mEnableFilter = false;
        // also cleanup if previously existed
        if (mFilterGraph) {
            avfilter_graph_free(&mFilterGraph);
            mFilterGraph = nullptr;
            mBuffersrcCtx = nullptr;
            mBuffersinkCtx = nullptr;
        }
        mFilterInited = false;
    }
    return mDemuxer.openStream(url, this, repeat);
}

int StreamDecoder::closeStream(bool isWaiting)
{
    return mDemuxer.closeStream(isWaiting);
}

AVPacket *StreamDecoder::ffmpegPacketAlloc()
{
#if LIBAVCODEC_VERSION_MAJOR > 56
    AVPacket *pktNew = av_packet_alloc();
#else
    AVPacket *pktNew = (AVPacket *)av_malloc(sizeof(AVPacket));
    av_init_packet(pktNew);
#endif
    return pktNew;
}

AVCodecContext *StreamDecoder::ffmpegCreateDecoder(enum AVCodecID codecId, AVDictionary **opts)
{
    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (NULL == codec)
    {
        printf("can't find code_id %d\n", codecId);
        return nullptr;
    }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    if (decCtx == NULL)
    {
        printf("avcodec_alloc_context3 err");
        return nullptr;
    }
#if LIBAVCODEC_VERSION_MAJOR < 56
    if (codec->capabilities & AV_CODEC_CAP_TRUNCATED)
    {
        decCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
    }
#endif

    decCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    decCtx->workaround_bugs = FF_BUG_AUTODETECT;
    decCtx->err_recognition = AV_EF_CAREFUL;
    decCtx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    decCtx->has_b_frames = 0;

    if (avcodec_open2(decCtx, codec, opts) < 0)
    {
        std::cout << "Unable to open codec";
        avcodec_free_context(&decCtx);
        return nullptr;
    }

    return decCtx;
}

bool StreamDecoder::isKeyFrame(AVPacket *pkt)
{
    auto decCtx = mExternalDecCtx != nullptr ? mExternalDecCtx : mDecCtx;
    if (!decCtx || !pkt || !pkt->data || pkt->size <= 0) return false;

    const uint8_t *data = pkt->data;
    int size = pkt->size;

    auto has_start_code = [&](const uint8_t *p, int n) -> bool {
        if (n >= 3 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01) return true;
        if (n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x01) return true;
        return false;
    };

    auto parse_annexb = [&](auto &&on_nalu) {
        const uint8_t *p = data;
        const uint8_t *end = data + size;
        // find first start code
        while (p + 3 < end && !has_start_code(p, end - p)) p++;
        while (p + 3 < end) {
            // skip start code
            int sc = (p[2] == 0x01) ? 3 : 4;
            if (!has_start_code(p, end - p)) break;
            p += sc;
            if (p >= end) break;
            const uint8_t *nalu_start = p;
            // find next start code to get nalu_end
            const uint8_t *q = p;
            while (q + 3 < end && !has_start_code(q, end - q)) q++;
            const uint8_t *nalu_end = q;
            if (nalu_end > nalu_start) {
                on_nalu(nalu_start, (int)(nalu_end - nalu_start));
            }
            p = q;
        }
    };

    auto parse_avcc = [&](int nalu_hdr_bytes, auto &&on_nalu) {
        const uint8_t *p = data;
        const uint8_t *end = data + size;
        while (p + 4 <= end) {
            uint32_t beLen = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | (p[3]);
            p += 4;
            if (beLen == 0 || p + beLen > end) break;
            if ((int)beLen > nalu_hdr_bytes) {
                on_nalu(p, (int)beLen);
            }
            p += beLen;
        }
    };

    bool foundKey = false;
    if (decCtx->codec_id == AV_CODEC_ID_H264) {
        auto on_h264 = [&](const uint8_t *nalu, int n) {
            if (n < 1) return;
            uint8_t nalType = nalu[0] & 0x1F;
            // Skip SEI/SPS/PPS for keyframe decision
            if (nalType == 6 || nalType == 7 || nalType == 8) return;
            if (nalType == 5) { // IDR
                foundKey = true;
            }
        };
        if (has_start_code(data, size)) {
            parse_annexb(on_h264);
        } else {
            // assume 4-byte length prefix AVCC
            parse_avcc(1, on_h264);
        }
    } else if (decCtx->codec_id == AV_CODEC_ID_H265) {
        auto on_h265 = [&](const uint8_t *nalu, int n) {
            if (n < 2) return;
            uint8_t nalUnitType = (nalu[0] >> 1) & 0x3F;
            // Skip SEI prefix/suffix
            if (nalUnitType == 39 || nalUnitType == 40) return;
            // Treat IDR_W_RADL(19), IDR_N_LP(20), CRA(21) as keyframes for access-point frames
            if (nalUnitType == 19 || nalUnitType == 20 || nalUnitType == 21) {
                foundKey = true;
            }
        };
        if (has_start_code(data, size)) {
            parse_annexb(on_h265);
        } else {
            // assume 4-byte length prefix HVCC
            parse_avcc(2, on_h265);
        }
    } else {
        // For other codecs, fallback to packet flag if available
        foundKey = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    }

    return foundKey;
}

} // namespace otl

// -------------------- Internal helpers: filter graph --------------------
namespace otl {
int StreamDecoder::initFilterGraphWithFrame(AVCodecContext *decCtx, const AVFrame *sampleFrame)
{
    if (!mEnableFilter || mFilterDesc.empty()) return -1;

    int ret = 0;
    char args[512];
    AVFilterInOut *inputs = nullptr;
    AVFilterInOut *outputs = nullptr;
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");

    if (!buffersrc || !buffersink) return AVERROR_FILTER_NOT_FOUND;

    mFilterGraph = avfilter_graph_alloc();
    if (!mFilterGraph) return AVERROR(ENOMEM);

    // Note: Do not call avfilter_graph_set_device for compatibility with older FFmpeg.
    // HW context will be propagated via buffersrc hw_frames_ctx when available.

    AVRational tb = mTimebase.num ? mTimebase : av_make_q(1, 25);
    AVRational sar = sampleFrame->sample_aspect_ratio.num ? sampleFrame->sample_aspect_ratio : av_make_q(1,1);

    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             sampleFrame->width, sampleFrame->height, sampleFrame->format,
             tb.num, tb.den, sar.num, sar.den);

    if ((ret = avfilter_graph_create_filter(&mBuffersrcCtx, buffersrc, "in", args, nullptr, mFilterGraph)) < 0) {
        print_ffmpeg_error(ret);
        goto fail;
    }

    if ((ret = avfilter_graph_create_filter(&mBuffersinkCtx, buffersink, "out", nullptr, nullptr, mFilterGraph)) < 0) {
        print_ffmpeg_error(ret);
        goto fail;
    }

    // If sample frame is a HW frame, propagate its hw_frames_ctx to buffersrc
    if (sampleFrame->hw_frames_ctx) {
        AVBufferRef *hwf = av_buffer_ref(sampleFrame->hw_frames_ctx);
        if (hwf) {
            AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
            if (!par) { ret = AVERROR(ENOMEM); goto fail; }
            memset(par, 0, sizeof(*par));
            par->format = sampleFrame->format;
            par->time_base = tb;
            par->hw_frames_ctx = hwf;
            par->width = sampleFrame->width;
            par->height = sampleFrame->height;
            ret = av_buffersrc_parameters_set(mBuffersrcCtx, par);
            av_freep(&par);
            if (ret < 0) {
                print_ffmpeg_error(ret);
                goto fail;
            }
        }
    }

    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs) { ret = AVERROR(ENOMEM); goto fail; }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = mBuffersrcCtx;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = mBuffersinkCtx;
    inputs->pad_idx     = 0;
    inputs->next        = nullptr;

    if ((ret = avfilter_graph_parse_ptr(mFilterGraph, mFilterDesc.c_str(), &inputs, &outputs, nullptr)) < 0) {
        print_ffmpeg_error(ret);
        goto fail;
    }

    if ((ret = avfilter_graph_config(mFilterGraph, nullptr)) < 0) {
        print_ffmpeg_error(ret);
        goto fail;
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return 0;

fail:
    if (inputs) avfilter_inout_free(&inputs);
    if (outputs) avfilter_inout_free(&outputs);
    if (mFilterGraph) {
        avfilter_graph_free(&mFilterGraph);
        mBuffersrcCtx = nullptr;
        mBuffersinkCtx = nullptr;
    }
    return ret < 0 ? ret : -1;
}

int StreamDecoder::applyFilters(AVFrame *in, AVFrame *out)
{
    if (!mFilterGraph || !mBuffersrcCtx || !mBuffersinkCtx) return -1;
    int ret = av_buffersrc_add_frame_flags(mBuffersrcCtx, in, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) { print_ffmpeg_error(ret); return ret; }

    // Try to get one filtered frame
    ret = av_buffersink_get_frame(mBuffersinkCtx, out);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) print_ffmpeg_error(ret);
        return ret;
    }
    return 0;
}
}
