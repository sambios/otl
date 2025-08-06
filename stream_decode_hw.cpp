#include "stream_decode_hw.h"
#include "otl.h"
#include "stream_sei.h"

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
    }
    else if (strcmp(mszHWDevTypeName, "cuda") == 0) // CUDA HWAccel
    {
        strcpy(mszHWDecoderName, "h264_cuvid");
    }
}

StreamDecoder::~StreamDecoder()
{
    std::cout << "~StreamDecoder() dtor..." << std::endl;

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

                    // 如果是硬件帧，需要将其从硬件设备内存转移到系统内存
                    if (pFrame->format == mHwPixFmt)
                    {
                        AVFrame *sw_frame = av_frame_alloc();
                        if (!sw_frame)
                        {
                            return AVERROR(ENOMEM);
                        }

                        // 从 GPU 内存转到 CPU 内存
                        if ((receive_ret = av_hwframe_transfer_data(sw_frame, pFrame, 0)) < 0)
                        {
                            av_log(NULL, AV_LOG_ERROR, "Error transferring HW frame data to system memory\n");
                            av_frame_free(&sw_frame);
                            return receive_ret;
                        }

                        // 复制时间戳等元数据
                        av_frame_copy_props(sw_frame, pFrame);
                        av_frame_unref(pFrame);

                        // 将 sw_frame 数据复制回 pFrame
                        av_frame_move_ref(pFrame, sw_frame);
                        av_frame_free(&sw_frame);
                    }
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
        if (pFrame->format == mHwPixFmt)
        {
            AVFrame *sw_frame = av_frame_alloc();
            if (!sw_frame)
            {
                return AVERROR(ENOMEM);
            }

            // 从 GPU 内存转到 CPU 内存
            if ((ret = av_hwframe_transfer_data(sw_frame, pFrame, 0)) < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Error transferring HW frame data to system memory\n");
                av_frame_free(&sw_frame);
                return ret;
            }

            // 复制时间戳等元数据
            av_frame_copy_props(sw_frame, pFrame);
            av_frame_unref(pFrame);

            // 将 sw_frame 数据复制回 pFrame
            av_frame_move_ref(pFrame, sw_frame);
            av_frame_free(&sw_frame);
        }

        // 如果调用者期望只处理一帧，此处应该可以返回
        // 但为了支持一次解码多帧，我们继续处理直到需要更多数据
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

        if (pkt->data && 0 == pkt->data[0] && 0 == pkt->data[1] && 0 == pkt->data[2] && 1 == pkt->data[3])
        {
            // seiLen = h264_read_sei_rbsp(pkt->data + 4, pkt->size - 4, seiBufPtr.get(), pkt->size);
            seiLen = h264SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
        }

        if (seiLen > 0 && mOnDecodedSeiFunc)
        {
            mOnDecodedSeiFunc(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
        }
    }
    else if (decCtx->codec_id == AV_CODEC_ID_H265)
    {
        std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
        int seiLen = 0;

        if (pkt->data && 0 == pkt->data[0] && 0 == pkt->data[1] && 0 == pkt->data[2] && 1 == pkt->data[3])
        {
            // seiLen = h265_read_sei_rbsp(pkt->data + 4, pkt->size - 4, seiBufPtr.get(), pkt->size);
            seiLen = h265SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
            int nalType = 0;
            if (0 == pkt->data[0] && 0 == pkt->data[1] && 0 == pkt->data[2] && 1 == pkt->data[3])
            {
                nalType = (pkt->data[4] & 0x7E) >> 1;
            }
            else if (0 == pkt->data[0] && 0 == pkt->data[1] && 1 == pkt->data[2])
            {
                nalType = (pkt->data[3] & 0x7E) >> 1;
            }

            if (nalType == 39)
            {
                if (mObserver != nullptr || mOnDecodedSeiFunc != nullptr)
                {
                    std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
                    int seiLen = 0;

                    if (pkt->data && 0 == pkt->data[0] && 0 == pkt->data[1] && 0 == pkt->data[2] && 1 == pkt->data[3])
                    {
                        // seiLen = h264_read_sei_rbsp(pkt->data + 4, pkt->size - 4, seiBufPtr.get(), pkt->size);
                        seiLen = h264SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
                    }

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
            }
        }
    }

    // std::cout << __FUNCTION__ << ":" << __LINE__ << std::endl;
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

        if (mObserver)
        {
            mObserver->onDecodedAVFrame(pktS, frame);
        }

        if (mOnDecodedFrameFunc != nullptr)
        {
            mOnDecodedFrameFunc(pktS, frame);
        }

        av_packet_unref(pktS);
        av_freep(&pktS);
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
        av_dict_set(&mOptsDecoder, "output_pixfmt", "yuv420p", 0);

        mDecCtx->hw_device_ctx = av_buffer_ref(mHWDeviceCtx);
        mDecCtx->opaque = this;
        mDecCtx->get_format = get_hw_format;
        mDecCtx->pix_fmt = AV_PIX_FMT_YUV420P; // 可以直接指定。
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
    if (decCtx == nullptr)
        return false;
    if (decCtx->codec_id == AV_CODEC_ID_H264)
    {
        if (pkt == nullptr || pkt->data == nullptr)
            return false;
        int nalType = pkt->data[4] & 0x1f;
        if (nalType != 7)
        {
            uint8_t *p = &pkt->data[0];
            uint8_t *end = &pkt->data[pkt->size];
            p += 4;
            while (p != end)
            {
                if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
                {
                    nalType = p[4] & 0x1f;
                    break;
                }
                p++;
            }
        }
        if (nalType == 7 || nalType == 5)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return true;
    }
}

} // namespace otl
