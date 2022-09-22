#include "common.hpp"
#include <vector>
#include <string>

namespace ffutils {

int Decode(
    AVCodecContext * avctx,
    AVFrame *        frame,
    int *            gotFrame,
    AVPacket *       pkt
) {
    int ret;
    int consumed = 0;
    *gotFrame = 0;
    // This relies on the fact that the decoder will not buffer additional
    // packets internally, but returns AVERROR(EAGAIN) if there are still
    // decoded frames to be returned.
    {
        // Timeit _("send packet");
        ret = avcodec_send_packet(avctx, pkt);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;
    if (ret >= 0) consumed = pkt->size;
    {
        // Timeit _("receive frame");
        ret = avcodec_receive_frame(avctx, frame);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;
    if (ret >= 0) *gotFrame = 1;
    return consumed;
}

int Encode(
    AVCodecContext * avctx,
    AVPacket *       pkt,
    int *            gotPacket,
    AVFrame *        frame
) {
    int ret;
    *gotPacket = 0;
    ret = avcodec_send_frame(avctx, frame);
    if (ret < 0 && frame) return ret;
    ret = avcodec_receive_packet(avctx, pkt);
    if (!ret) *gotPacket = 1;
    if (ret == AVERROR(EAGAIN)) return 0;
    return ret;
}

#if LIBAVUTIL_VERSION_MAJOR >= 57
AVFrame *AllocateFrame(
    enum AVSampleFormat     sampleFmt,
    AVChannelLayout const & channelLayout,
    int                     sampleRate,
    int                     nbSamples
) {
    AVFrame *frame = av_frame_alloc();
    if (!frame) { snow::log::critical("[ffmpeg] Error allocating an audio fram"); }
    frame->format      = sampleFmt;
    frame->ch_layout   = channelLayout;
    frame->sample_rate = sampleRate;
    frame->nb_samples  = nbSamples;
    frame->pts         = AV_NOPTS_VALUE;
    if (nbSamples) {
        int ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) { snow::log::critical("[ffmpeg] Error allocating an audio buffer"); }
    }
    return frame;
}
#else
AVFrame *AllocateFrame(
    enum AVSampleFormat sampleFmt,
    uint64_t            channelLayout,
    int                 sampleRate,
    int                 nbSamples
) {
    AVFrame *frame = av_frame_alloc();
    if (!frame) { snow::log::critical("[ffmpeg] Error allocating an audio fram"); }
    frame->format         = sampleFmt;
    frame->channel_layout = channelLayout;
    frame->sample_rate    = sampleRate;
    frame->nb_samples     = nbSamples;
    frame->pts            = AV_NOPTS_VALUE;
    if (nbSamples) {
        int ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) { snow::log::critical("[ffmpeg] Error allocating an audio buffer"); }
    }
    return frame;
}
#endif

AVFrame *AllocateFrame(
    enum AVPixelFormat pixFmt,
    int width, int height
) {
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture) return nullptr;

    picture->format = pixFmt;
    picture->width  = width;
    picture->height = height;
    picture->pts    = AV_NOPTS_VALUE;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) { snow::log::critical("[ffmpeg] Could not allocate frame data: {}", av_err2str(ret)); }

    return picture;
}

std::vector<float> Resample(const std::vector<float> &audio, int srcSampleRate, int dstSampleRate) {
    uint8_t **srcData = NULL, **dstData = NULL;
    int srcNumChannels = 0, dstNumChannels = 0;
    int srcLineSize, dstLineSize;
    int srcNumSamples = 1024, dstNumSamples, maxDstNumSamples;
    int dstBufferSize;
    int index = 0;
    int ret;
    struct SwrContext *swrCtxPtr;
    std::vector<float> resampledAudio;

    do {
        /* create resampler context */
        swrCtxPtr = swr_alloc();
        if (!swrCtxPtr) {
            snow::log::error("[ffmpeg] Could not allocate resampler context");
            ret = AVERROR(ENOMEM);
            break; // goto end;
        }

        /* set options */
        av_opt_set_int(swrCtxPtr, "in_channel_layout",     AV_CH_LAYOUT_MONO, 0);
        av_opt_set_int(swrCtxPtr, "in_sample_rate",        srcSampleRate,     0);
        av_opt_set_sample_fmt(swrCtxPtr, "in_sample_fmt",  AV_SAMPLE_FMT_FLT, 0);

        av_opt_set_int(swrCtxPtr, "out_channel_layout",    AV_CH_LAYOUT_MONO, 0);
        av_opt_set_int(swrCtxPtr, "out_sample_rate",       dstSampleRate,     0);
        av_opt_set_sample_fmt(swrCtxPtr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

        /* initialize the resampling context */
        if ((ret = swr_init(swrCtxPtr)) < 0) {
            snow::log::error("[ffmpeg] Failed to initialize the resampling context");
            break; // goto end;
        }

        /* allocate source and destination samples buffers */
#if LIBAVUTIL_VERSION_MAJOR >= 57
        srcNumChannels = AVChannelLayout(AV_CHANNEL_LAYOUT_MONO).nb_channels;
#else
        srcNumChannels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_MONO);
#endif
        ret = av_samples_alloc_array_and_samples(&srcData, &srcLineSize, srcNumChannels,
                                                 srcNumSamples, AV_SAMPLE_FMT_FLT, 0);
        if (ret < 0) {
            snow::log::error("[ffmpeg] Could not allocate source samples");
            break;  // goto end;
        }

        /* compute the number of converted samples: buffering is avoided
        * ensuring that the output buffer will contain at least all the
        * converted input samples */
        maxDstNumSamples = dstNumSamples = av_rescale_rnd(srcNumSamples, dstSampleRate, srcSampleRate, AV_ROUND_UP);

        /* buffer is going to be directly written to a rawaudio file, no alignment */
#if LIBAVUTIL_VERSION_MAJOR >= 57
        dstNumChannels = AVChannelLayout(AV_CHANNEL_LAYOUT_MONO).nb_channels;
#else
        dstNumChannels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_MONO);
#endif
        ret = av_samples_alloc_array_and_samples(&dstData, &dstLineSize, dstNumChannels,
                                                 dstNumSamples, AV_SAMPLE_FMT_FLT, 0);
        if (ret < 0) {
            snow::log::error("[ffmpeg] Could not allocate destination samples");
            break; /* goto end; */
        }

        while (index + srcNumSamples <= (int)audio.size()) {
            /* generate synthetic audio */
            for (int si = 0; si < srcNumSamples; ++si) {
                ((float *)srcData[0])[si] = audio[index++];
            }

            /* compute destination number of samples */
            dstNumSamples = av_rescale_rnd(swr_get_delay(swrCtxPtr, srcSampleRate) +
                                            srcNumSamples, dstSampleRate, srcSampleRate, AV_ROUND_UP);
            if (dstNumSamples > maxDstNumSamples) {
                av_freep(&dstData[0]);
                ret = av_samples_alloc(dstData, &dstLineSize, dstNumChannels,
                                       dstNumSamples, AV_SAMPLE_FMT_FLT, 1);
                if (ret < 0) break;
                maxDstNumSamples = dstNumSamples;
            }

            /* convert to destination format */
            ret = swr_convert(swrCtxPtr, dstData, dstNumSamples, (const uint8_t **)srcData, srcNumSamples);
            if (ret < 0) {
                snow::log::error("[ffmpeg] Error while converting");
                break; /* goto end; */
            }
            dstBufferSize = av_samples_get_buffer_size(&dstLineSize, dstNumChannels, ret, AV_SAMPLE_FMT_FLT, 1);
            if (dstBufferSize < 0) {
                snow::log::error("[ffmpeg] Could not get sample buffer size");
                break; /* goto end; */
            }
            for (int si = 0; si < ret; ++si) {
                resampledAudio.push_back(((float *)dstData[0])[si]);
            }
        }

    } while (false);

    // end 
    if (srcData) av_freep(&srcData[0]);
    av_freep(&srcData);

    if (dstData) av_freep(&dstData[0]);
    av_freep(&dstData);

    swr_free(&swrCtxPtr);
    return resampledAudio;
}

}
