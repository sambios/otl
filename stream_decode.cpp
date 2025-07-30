#include "stream_decode.h"
#include "stream_sei.h"

namespace otl {

StreamDecoder::StreamDecoder(int id, AVCodecContext *decoder)
    : m_observer(nullptr), m_externalDecCtx(decoder) {
    std::cout << "StreamDecoder() ctor..." << std::endl;
    m_isWaitingIframe = true;
    m_id = id;
    m_optsDecoder = nullptr;
}

StreamDecoder::~StreamDecoder() {
    std::cout << "~StreamDecoder() dtor..." << std::endl;
    av_dict_free(&m_optsDecoder);
}

int StreamDecoder::decodeFrame(AVPacket *pkt, AVFrame *pFrame) {
    AVCodecContext *decCtx = nullptr;
    if (nullptr == m_externalDecCtx) {
        decCtx = m_decCtx;
    } else {
        decCtx = m_externalDecCtx;
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
    if (m_pfnOnAVFormatOpened != nullptr) {
        m_pfnOnAVFormatOpened(ifmtCtx);
    }

    if (m_externalDecCtx == nullptr) {
        if (0 == createVideoDecoder(ifmtCtx)) {
            printf("create video decoder ok!\n");
        }
    }

    if (strcmp(ifmtCtx->iformat->name, "h264") != 0) {
        m_isWaitingIframe = false;
    }
}

void StreamDecoder::onAvformatClosed() {
    clearPackets();
    if (m_decCtx != nullptr) {
        avcodec_close(m_decCtx);
        avcodec_free_context(&m_decCtx);
        printf("free video decoder context!\n");
    }

    if (m_pfnOnAVFormatClosed) {
        m_pfnOnAVFormatClosed();
    }
}

int StreamDecoder::onReadFrame(AVPacket *pkt) {
    int ret = 0;

    if (m_videoStreamIndex != pkt->stream_index) {
        // ignore other streams if not video.
        return 0;
    }

    if (m_isWaitingIframe) {
        if (isKeyFrame(pkt)) {
            m_isWaitingIframe = false;
        }
    }

    if (m_isWaitingIframe) {
        return 0;
    }

    auto decCtx = m_externalDecCtx != nullptr ? m_externalDecCtx : m_decCtx;

    if (decCtx->codec_id == AV_CODEC_ID_H264) {
        std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
        int seiLen = 0;

        if (pkt->data &&
            0 == pkt->data[0] &&
            0 == pkt->data[1] &&
            0 == pkt->data[2] &&
            1 == pkt->data[3] &&
            (pkt->data[4] & 0x1f) == 6) {

            if (m_onDecodedSEIFunc != nullptr || m_observer != nullptr) {
                seiLen = h264SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
                if (seiLen > 0) {
                    if (m_observer != nullptr) {
                        m_observer->onDecodedSeiInfo(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
                    }

                    if (m_onDecodedSEIFunc != nullptr) {
                        m_onDecodedSEIFunc(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
                    }
                }
            }
        }
    } else if (decCtx->codec_id == AV_CODEC_ID_H265) {
        std::unique_ptr<uint8_t[]> seiBufPtr(new uint8_t[pkt->size]);
        int seiLen = 0;

        if (pkt->data) {
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
                if (m_observer != nullptr || m_onDecodedSEIFunc != nullptr) {
                    seiLen = h264SeiPacketRead(pkt->data, pkt->size, seiBufPtr.get(), pkt->size);
                    if (seiLen > 0) {
                        if (m_observer != nullptr) {
                            m_observer->onDecodedSeiInfo(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
                        }

                        if (m_onDecodedSEIFunc != nullptr) {
                            m_onDecodedSEIFunc(seiBufPtr.get(), seiLen, pkt->pts, pkt->pos);
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

    if (m_frameDecodedNum == 0) {
        printf("id=%d, ffmpeg delayed frames: %d\n", m_id, (int)m_listPackets.size());
    }

    if (ret > 0) m_frameDecodedNum++;

    putPacket(pkt);

    if (ret > 0) {
        auto pktS = getPacket();

        if (m_observer) {
            m_observer->onDecodedAVFrame(pktS, frame);
        }

        if (m_onDecodedFrameFunc != nullptr) {
            m_onDecodedFrameFunc(pktS, frame);
        }

        av_packet_unref(pktS);
        av_freep(&pktS);
    }

    av_frame_unref(frame);
    av_frame_free(&frame);

    return ret;
}

void StreamDecoder::onReadEof(AVPacket *pkt) {
#if 1
    while (1) {
        int ret = onReadFrame(pkt);
        if (ret <= 0) {
            break;
        }
    }
#endif
    m_frameDecodedNum = 0;
    clearPackets();

    if (m_observer) {
        m_observer->onStreamEof();
    }

    if (m_pfnOnReadEof != nullptr) {
        m_pfnOnReadEof(nullptr);
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
    m_listPackets.push_back(pktNew);
    return 0;
}

AVPacket *StreamDecoder::getPacket() {
    if (m_listPackets.size() == 0) return nullptr;
    auto pkt = m_listPackets.front();
    m_listPackets.pop_front();
    return pkt;
}

void StreamDecoder::clearPackets() {
    while (m_listPackets.size() > 0) {
        auto pkt = m_listPackets.front();
        m_listPackets.pop_front();
        av_packet_unref(pkt);
        av_freep(&pkt);
    }
    return;
}

int StreamDecoder::getVideoStreamIndex(AVFormatContext *ifmtCtx) {
    for (unsigned int i = 0; i < ifmtCtx->nb_streams; i++) {
#if LIBAVFORMAT_VERSION_MAJOR > 56
        auto codecType = ifmtCtx->streams[i]->codecpar->codec_type;
#else
        auto codecType = ifmtCtx->streams[i]->codec->codec_type;
#endif
        if (codecType == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = i;
            break;
        }
    }
    return m_videoStreamIndex;
}

AVCodecID StreamDecoder::getVideoCodecId() {
    if (m_decCtx) {
        return m_decCtx->codec_id;
    }
    return AV_CODEC_ID_NONE;
}

int StreamDecoder::createVideoDecoder(AVFormatContext *ifmtCtx) {
    int videoIndex = getVideoStreamIndex(ifmtCtx);
    m_timebase = ifmtCtx->streams[videoIndex]->time_base;

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

    m_decCtx = avcodec_alloc_context3(codec);
    if (m_decCtx == NULL) {
        printf("avcodec_alloc_context3 err");
        return -1;
    }

    int ret = 0;

#if LIBAVCODEC_VERSION_MAJOR > 56
    if ((ret = avcodec_parameters_to_context(m_decCtx, ifmtCtx->streams[videoIndex]->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }
#else
    if ((ret = avcodec_copy_context(m_decCtx, ifmtCtx->streams[videoIndex]->codec)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }
#endif

    if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
        m_decCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
    }

    AVDictionary *opts = NULL;
    av_dict_copy(&opts, m_optsDecoder, 0);
    if (avcodec_open2(m_decCtx, codec, &opts) < 0) {
        std::cout << "Unable to open codec";
        return -1;
    }

    return 0;
}

int StreamDecoder::setObserver(StreamDecoderEvents *observer) {
    m_observer = observer;
    return 0;
}

int StreamDecoder::openStream(std::string url, bool repeat, AVDictionary *opts) {
    av_dict_copy(&m_optsDecoder, opts, 0);
    return m_demuxer.openStream(url, this, repeat);
}

int StreamDecoder::closeStream(bool isWaiting) {
    return m_demuxer.closeStream(isWaiting);
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

    if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
        decCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
    }

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
    auto decCtx = m_externalDecCtx != nullptr ? m_externalDecCtx : m_decCtx;
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

