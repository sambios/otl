#ifndef OTL_IMAGE_H
#define OTL_IMAGE_H

#ifdef __cplusplus
extern "C" {
#include "libavutil/avutil.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
}
#endif

namespace otl
{

  static inline int convert_yuv420p_software(const AVFrame *src, AVFrame **p_dst)
  {
    AVFrame *dst = av_frame_alloc();
    dst->width = src->width;
    dst->height = src->height;
    dst->format = AV_PIX_FMT_YUV420P;

    av_frame_get_buffer(dst, 64);
    SwsContext *ctx = sws_getContext(src->width, src->height, (AVPixelFormat)src->format, dst->width, dst->height,
                                     (AVPixelFormat)dst->format, SWS_BICUBIC, 0, NULL, NULL);
    sws_scale(ctx, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    sws_freeContext(ctx);
    *p_dst = dst;
    return 0;
  }

  static inline int convert_yuv420p_to_nv12(const AVFrame *src, AVFrame **p_dst)
  {
    AVFrame *dst = av_frame_alloc();
    dst->width = src->width;
    dst->height = src->height;
    dst->format = AV_PIX_FMT_NV12;

    av_frame_get_buffer(dst, 64);
    SwsContext *ctx = sws_getContext(src->width, src->height, (AVPixelFormat)src->format, dst->width, dst->height,
                                     (AVPixelFormat)dst->format, SWS_BICUBIC, 0, NULL, NULL);
    sws_scale(ctx, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    sws_freeContext(ctx);
    *p_dst = dst;
    return 0;
  }
}

#endif // OTL_IMAGE_H