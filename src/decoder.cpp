#include "decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spdlog/spdlog.h>

static enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;

// Callback function to choose the hardware-accelerated pixel format
static enum AVPixelFormat get_hw_format(AVCodecContext *_, const enum AVPixelFormat *pix_fmts) {
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == hw_pix_fmt) {
            return *p;
        }
    }
    spdlog::error("Failed to get HW surface format.");
    return AV_PIX_FMT_NONE;
}

int init_decoder(
    AVHWDeviceType hw_type,
    AVBufferRef *hw_ctx,
    std::filesystem::path in_fpath,
    AVFormatContext **fmt_ctx,
    AVCodecContext **dec_ctx,
    int *in_vstream_idx
) {
    AVFormatContext *ifmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    int ret;

    if ((ret = avformat_open_input(&ifmt_ctx, in_fpath.u8string().c_str(), NULL, NULL)) < 0) {
        spdlog::error("Could not open input file '{}'", in_fpath.u8string().c_str());
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        spdlog::error("Failed to retrieve input stream information");
        return ret;
    }

    // Find the first video stream
    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        spdlog::error("Could not find video stream in the input file");
        return ret;
    }

    int stream_index = ret;
    AVStream *video_stream = ifmt_ctx->streams[stream_index];

    // Set up the decoder
    const AVCodec *decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!decoder) {
        spdlog::error(
            "Failed to find decoder for codec ID {}",
            static_cast<int>(video_stream->codecpar->codec_id)
        );
        return AVERROR_DECODER_NOT_FOUND;
    }

    codec_ctx = avcodec_alloc_context3(decoder);
    if (!codec_ctx) {
        spdlog::error("Failed to allocate the decoder context");
        return AVERROR(ENOMEM);
    }

    // Set hardware device context
    if (hw_ctx != nullptr) {
        codec_ctx->hw_device_ctx = av_buffer_ref(hw_ctx);
        codec_ctx->get_format = get_hw_format;

        // Automatically determine the hardware pixel format
        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (config == nullptr) {
                spdlog::error(
                    "Decoder {} does not support device type {}.",
                    decoder->name,
                    av_hwdevice_get_type_name(hw_type)
                );
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&ifmt_ctx);
                return AVERROR(ENOSYS);
            }
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == hw_type) {
                hw_pix_fmt = config->pix_fmt;
                break;
            }
        }
    }

    if ((ret = avcodec_parameters_to_context(codec_ctx, video_stream->codecpar)) < 0) {
        spdlog::error("Failed to copy decoder parameters to input decoder context");
        return ret;
    }

    // Set decoder time base and frame rate
    codec_ctx->time_base = video_stream->time_base;
    codec_ctx->pkt_timebase = video_stream->time_base;
    codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, video_stream, NULL);

    if ((ret = avcodec_open2(codec_ctx, decoder, NULL)) < 0) {
        spdlog::error("Failed to open decoder for stream #{}", stream_index);
        return ret;
    }

    *fmt_ctx = ifmt_ctx;
    *dec_ctx = codec_ctx;
    *in_vstream_idx = stream_index;

    return 0;
}
