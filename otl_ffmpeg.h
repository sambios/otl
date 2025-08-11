//
// Created by hsyuan on 2019-03-15.
//

#ifndef OTL_FFMPEG_H
#define OTL_FFMPEG_H

#ifdef __cplusplus
extern "C" {
#include "libavdevice/avdevice.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/time.h"
#include "libavutil/imgutils.h"
}
#endif

#include <iostream>

namespace otl {

class FfmpegGlobal {
public:
    FfmpegGlobal() {
#if LIBAVCODEC_VERSION_MAJOR <= 56
        av_register_all();
#endif
        avformat_network_init();
        avdevice_register_all();
        av_log_set_level(AV_LOG_VERBOSE);
    }

    virtual ~FfmpegGlobal() {
        std::cout << "~FfmpegGlobal() dtor.." << std::endl;
        avformat_network_deinit();
    }
};

} // namespace otl

#endif // OTL_FFMPEG_H
