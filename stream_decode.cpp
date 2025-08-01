#include "stream_decode.h"
#include "stream_sei.h"

namespace otl {

StreamDecoder::StreamDecoder(int id, AVCodecContext *decoder)
    : mObserver(nullptr), mExternalDecCtx(decoder) {
    std::cout << "StreamDecoder() ctor..." << std::endl;
    mIsWaitingIframe = true;
    mId = id;
    mOptsDecoder = nullptr;
}

StreamDecoder::~StreamDecoder() {
    std::cout << "~StreamDecoder() dtor..." << std::endl;
    av_dict_free(&mOptsDecoder);
}

int StreamDecoder::decodeFrame(AVPacket *pkt, AVFrame *pFrame) {
    AVCodecContext *decCtx = nullptr;
    if (nullptr == mExternalDecCtx) {
        decCtx = mDecCtx;
    } else {
        decCtx = mExternalDecCtx;
    }

#if LIBAVCODEC_VERSION_MAJOR > 56
    int gotPicture = 0;
    int ret = avcodec_send_packet(decCtx, pkt);
    if (ret == AVERROR_EOF) ret = 0;
    else if (ret < 0) {
        printf(" error sending a packet for decoding\n");
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(decCtx, pFrame);
        if (ret == AVERROR(EAGAIN)) {
            printf("decoder need more stream!\n");
            break;
        } else if (ret == AVERROR_EOF) {
            printf("avcodec_receive_frame() err=end of file!\n");
        }

        if (0 == ret) {
            gotPicture += 1;
            break;
        }
    }

    return gotPicture;

#else
    int gotFrame = 0;
    int ret = avcodec_decode_video2(decCtx, pFrame, &gotFrame, pkt);
    if (ret < 0) {
        return -1;
    }

    if (gotFrame > 0) {
        return 1;
    }

    return 0;
#endif
}

void StreamDecoder::onAvformatOpened(AVFormatContext *ifmtCtx) {
    if (mOnAvformatOpenedFunc != nullptr) {
        mOnAvformatOpenedFunc(ifmtCtx);
    }

    if (mExternalDecCtx == nullptr) {
        if (0 == createVideoDecoder(ifmtCtx)) {
            printf("create video decoder ok!\n");
        }
    }

    if (strcmp(ifmtCtx->iformat->name, "h264") != 0) {
        mIsWaitingIframe = false;
    }
}

void StreamDecoder::onAvformatClosed() {
    clearPackets();
    if (mDecCtx != nullptr) {
        avcodec_close(mDecCtx);
        avcodec_free_context(&mDecCtx);
        printf("free video decoder context!\n");
    }

    if (mOnAvformatClosedFunc) mOnAvformatClosedFunc();
}

int StreamDecoder::onReadFrame(AVPacket *pkt) {
    int ret = 0;

    if (mVideoStreamIndex != pkt->stream_index) {
        // ignore other streams if not video.
        return 0;
    }

    if (mIsWaitingIframe) {
        if (isKeyFrame(pkt)) {
            mIsWaitingIframe = false;
        }
    }

    if (mIsWaitingIframe) {
        return 0;
    }

    auto decCtx = mExternalDecCtx != nullptr ? mExternalDecCtx : mDecCtx;

    if (decCtx->codec_id == AV_CODEC_ID_H264) {
        std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
        int seiLen = 0;

        if (pkt->data &&
            0 == pkt->data[0] &&
            0 == pkt->data[1] &&
            0 == pkt->data[2] &&
            1 == pkt->data[3]) {
            //seiLen = h264_read_sei_rbsp(pkt->data + 4, pkt->size - 4, seiBufPtr.get(), pkt->size);
            seiLen = h264SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
        }

        if (seiLen > 0 && mOnDecodedSeiFunc) {
            mOnDecodedSeiFunc(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
        }
    } else if (decCtx->codec_id == AV_CODEC_ID_H265) {
        std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
        int seiLen = 0;

        if (pkt->data &&
            0 == pkt->data[0] &&
            0 == pkt->data[1] &&
            0 == pkt->data[2] &&
            1 == pkt->data[3]) {
            //seiLen = h265_read_sei_rbsp(pkt->data + 4, pkt->size - 4, seiBufPtr.get(), pkt->size);
            seiLen = h265SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
            int nalType = 0;
            if (0 == pkt->data[0] &&
                0 == pkt->data[1] &&
                0 == pkt->data[2] &&
                1 == pkt->data[3]) {
                nalType = (pkt->data[4] & 0x7E) >> 1;
            } else if (0 == pkt->data[0] &&
                       0 == pkt->data[1] &&
                       1 == pkt->data[2]) {
                nalType = (pkt->data[3] & 0x7E) >> 1;
            }

            if (nalType == 39) {
                if (mObserver != nullptr || mOnDecodedSeiFunc != nullptr) {
                    std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
                    int seiLen = 0;

                    if (pkt->data &&
                        0 == pkt->data[0] &&
                        0 == pkt->data[1] &&
                        0 == pkt->data[2] &&
                        1 == pkt->data[3]) {
                        //seiLen = h264_read_sei_rbsp(pkt->data + 4, pkt->size - 4, seiBufPtr.get(), pkt->size);
                        seiLen = h264SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
                    }

                    if (seiLen > 0) {
                        if (mObserver != nullptr) {
                            mObserver->onDecodedSeiInfo(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
                        }

                        if (mOnDecodedSeiFunc != nullptr) {
                            mOnDecodedSeiFunc(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
                        }
                    }
                }
            }
        }
    }

    AVFrame *frame = av_frame_alloc();
    ret = decodeFrame(pkt, frame);

    if (ret < 0) {
        printf("decode failed!\n");
        av_frame_free(&frame);
        return ret;
    }

    if (mFrameDecodedNum == 0) {
        printf("id=%d, ffmpeg delayed frames: %d\n", mId, (int)mListPackets.size());
    }

    if (ret > 0) mFrameDecodedNum++;

    putPacket(pkt);

    if (ret > 0) {
        auto pktS = getPacket();

        if (mObserver) {
            mObserver->onDecodedAVFrame(pktS, frame);
        }

        if (mOnDecodedFrameFunc != nullptr) {
            mOnDecodedFrameFunc(pktS, frame);
        }

        av_packet_unref(pktS);
        av_freep(&pktS);
    }

    av_frame_unref(frame);
    av_frame_free(&frame);

    return ret;
}

void StreamDecoder::onReadEof(AVPacket *pkt) {
    mFrameDecodedNum = 0;
    clearPackets();

    if (mObserver) {
        mObserver->onStreamEof();
    }

    if (mOnReadEofFunc != nullptr) {
        mOnReadEofFunc(nullptr);
    }
}

int StreamDecoder::putPacket(AVPacket *pkt) {
#if LIBAVCODEC_VERSION_MAJOR > 56
    AVPacket *pktNew = av_packet_alloc();
#else
    AVPacket *pktNew = (AVPacket*)av_malloc(sizeof(AVPacket));
    av_init_packet(pktNew);
#endif

    av_packet_ref(pktNew, pkt);
    mListPackets.push_back(pktNew);
    return 0;
}

AVPacket *StreamDecoder::getPacket() {
    if (mListPackets.size() == 0) return nullptr;
    auto pkt = mListPackets.front();
    mListPackets.pop_front();
    return pkt;
}

void StreamDecoder::clearPackets() {
    while (mListPackets.size() > 0) {
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

int StreamDecoder::getVideoStreamIndex(AVFormatContext *ifmtCtx) {
    for (unsigned int i = 0; i < ifmtCtx->nb_streams; i++) {
#if LIBAVFORMAT_VERSION_MAJOR > 56
        auto codecType = ifmtCtx->streams[i]->codecpar->codec_type;
#else
        auto codecType = ifmtCtx->streams[i]->codec->codec_type;
#endif
        if (codecType == AVMEDIA_TYPE_VIDEO) {
            mVideoStreamIndex = i;
            break;
        }
    }
    return mVideoStreamIndex;
}

AVCodecID StreamDecoder::getVideoCodecId() {
    if (mDecCtx) {
        return mDecCtx->codec_id;
    }
    return AV_CODEC_ID_NONE;
    }

    int ret = 0;

int StreamDecoder::createVideoDecoder(AVFormatContext *ifmtCtx) {
    int videoIndex = getVideoStreamIndex(ifmtCtx);
    mTimebase = ifmtCtx->streams[videoIndex]->time_base;

#if LIBAVCODEC_VERSION_MAJOR > 56
    auto codecId = ifmtCtx->streams[videoIndex]->codecpar->codec_id;
#else
    auto codecId = ifmtCtx->streams[videoIndex]->codec->codec_id;
#endif

    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (NULL == codec) {
        printf("can't find code_id %d\n", codecId);
        return -1;
    }

    if (mDecCtx != nullptr) {
        avcodec_close(mDecCtx);
        avcodec_free_context(&mDecCtx);
        mDecCtx = nullptr;
    }

    mDecCtx = avcodec_alloc_context3(codec);
    if (mDecCtx == NULL) {
        printf("avcodec_alloc_context3 err");
        return -1;
    }

    int ret = 0;

#if LIBAVCODEC_VERSION_MAJOR > 56
    if ((ret = avcodec_parameters_to_context(mDecCtx, ifmtCtx->streams[videoIndex]->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }
#else
    if ((ret = avcodec_copy_context(mDecCtx, ifmtCtx->streams[videoIndex]->codec)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }

    if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
        mDecCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
    }
#endif

    if ((ret = avcodec_open2(mDecCtx, codec, &mOptsDecoder)) < 0) {
        fprintf(stderr, "Failed to open %s codec\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }

    return 0;
}

int StreamDecoder::setObserver(StreamDecoderEvents *observer) {
    mObserver = observer;
    return 0;
}

int StreamDecoder::openStream(std::string url, bool repeat, AVDictionary *opts) {
    av_dict_copy(&mOptsDecoder, opts, 0);
    return mDemuxer.openStream(url, this, repeat);
}

int StreamDecoder::closeStream(bool isWaiting) {
    return mDemuxer.closeStream(isWaiting);
}

AVPacket* StreamDecoder::ffmpegPacketAlloc() {
#if LIBAVCODEC_VERSION_MAJOR > 56
    AVPacket *pktNew = av_packet_alloc();
#else
    AVPacket *pktNew = (AVPacket*)av_malloc(sizeof(AVPacket));
    av_init_packet(pktNew);
#endif
    return pktNew;
}

AVCodecContext* StreamDecoder::ffmpegCreateDecoder(enum AVCodecID codecId, AVDictionary **opts) {
    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (NULL == codec) {
        printf("can't find code_id %d\n", codecId);
        return nullptr;
    }

    AVCodecContext* decCtx = avcodec_alloc_context3(codec);
    if (decCtx == NULL) {
        printf("avcodec_alloc_context3 err");
        return nullptr;
    }
#if LIBAVCODEC_VERSION_MAJOR < 56
    if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
        decCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
    }
#endif

    decCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    decCtx->workaround_bugs = FF_BUG_AUTODETECT;
    decCtx->err_recognition = AV_EF_CAREFUL;
    decCtx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    decCtx->has_b_frames = 0;

    if (avcodec_open2(decCtx, codec, opts) < 0) {
        std::cout << "Unable to open codec";
        avcodec_free_context(&decCtx);
        return nullptr;
    }

    return decCtx;
}

bool StreamDecoder::isKeyFrame(AVPacket *pkt) {
    auto decCtx = mExternalDecCtx != nullptr ? mExternalDecCtx : mDecCtx;
    if (decCtx->codec_id == AV_CODEC_ID_H264) {
        if (pkt == nullptr || pkt->data == nullptr) return false;
        int nalType = pkt->data[4] & 0x1f;
        if (nalType != 7) {
            uint8_t *p = &pkt->data[0];
            uint8_t *end = &pkt->data[pkt->size];
            p += 4;
            while (p != end) {
                if (p[0] == 0 && p[1] == 0 &&
                    p[2] == 0 && p[3] == 1) {
                    nalType = p[4] & 0x1f;
                    break;
                }
                p++;
            }
        }
        if (nalType == 7 || nalType == 5) {
            return true;
        } else {
            return false;
        }
    } else {
        return true;
    }
}

} // namespace otl

