/*
 * ffpipenode_android_mediacodec_vdec.c
 *
 * Copyright (c) 2014 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ffpipenode_android_mediacodec_vdec.h"
#include "ffpipeline_android.h"
#include "../../ff_ffpipenode.h"
#include "../../ff_ffplay.h"
#include "ijksdl/android/ijksdl_android_jni.h"
#include "ijksdl/android/ijksdl_codec_android_mediaformat_java.h"
#include "ijksdl/android/ijksdl_codec_android_mediacodec_java.h"
#include "ijksdl/android/ijksdl_vout_android_nativewindow.h"
#include "ijksdl/android/ijksdl_vout_overlay_android_mediacodec.h"
#include "h264_nal.h"

#define AMC_USE_AVBITSTREAM_FILTER 0
#ifndef AMCTRACE
#define AMCTRACE(...)
#endif

typedef struct IJKFF_Pipenode_Opaque {
    FFPlayer                 *ffp;
    IJKFF_Pipeline           *pipeline;
    Decoder                  *decoder;
    SDL_Vout                 *weak_vout;

    const char               *codec_mime;

    jobject                   jsurface;
    SDL_AMediaFormat         *input_aformat;
    SDL_AMediaCodec          *acodec;
    SDL_AMediaFormat         *output_aformat;
    int                       frame_width;
    int                       frame_height;

    AVCodecContext           *avctx; // not own
    AVBitStreamFilterContext *bsfc;  // own

    size_t                    nal_size;
    uint8_t                  *orig_extradata;
    int                       orig_extradata_size;

    SDL_Thread               _enqueue_thread;
    SDL_Thread               *enqueue_thread;

    int                       abort_request;
    int                       input_error_count;
    int                       output_error_count;
} IJKFF_Pipenode_Opaque;

static int reconfigureMediaCodec(JNIEnv *env, IJKFF_Pipenode *node)
{
    IJKFF_Pipenode_Opaque *opaque   = node->opaque;
    IJKFF_Pipeline        *pipeline = opaque->pipeline;
    int                    ret      = 0;
    sdl_amedia_status_t    amc_ret  = 0;

    ffpipeline_set_surface_need_reconfigure(pipeline, false);

    SDL_JNI_DeleteGlobalRefP(env, &opaque->jsurface);
    opaque->jsurface = ffpipeline_get_surface_as_global_ref(env, pipeline);

    // need lock
    if (SDL_AMediaCodec_is_configured(opaque->acodec)) {
        // SDL_AMediaCodec_stop(opaque->acodec);

        if (opaque->acodec && SDL_AMediaCodec_is_configured(opaque->acodec)) {
            assert(opaque->weak_vout);
            SDL_VoutAndroid_setAMediaCodec(opaque->weak_vout, NULL);
            SDL_AMediaCodec_deleteP(&opaque->acodec);
        }

        opaque->acodec = SDL_AMediaCodecJava_createDecoderByType(env, opaque->codec_mime);
        if (!opaque->acodec) {
            ALOGE("%s:open_video_decoder: SDL_AMediaCodecJava_createDecoderByType failed\n", __func__);
            ret = -1;
            goto fail;
        }
        assert(opaque->weak_vout);
        SDL_VoutAndroid_setAMediaCodec(opaque->weak_vout, opaque->acodec);
    }

    amc_ret = SDL_AMediaCodec_configure_surface(env, opaque->acodec, opaque->input_aformat, opaque->jsurface, NULL, 0);
    if (amc_ret != SDL_AMEDIA_OK) {
        ALOGE("%s:configure_surface: failed\n", __func__);
        ret = -1;
        goto fail;
    }

    SDL_AMediaCodec_start(opaque->acodec);
fail:
    return ret;
}

// like ff_ffplay.c: free_picture()
static void amc_free_picture(Frame *vp)
{
    if (vp->bmp) {
        SDL_VoutFreeYUVOverlay(vp->bmp);
        vp->bmp = NULL;
    }
}

// like ff_ffplay.c: alloc_picture()
static void amc_alloc_picture(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    Frame *vp;

    vp = &is->pictq.queue[is->pictq.windex];

    amc_free_picture(vp);

    vp->bmp = SDL_Vout_CreateOverlay(vp->width, vp->height,
                                   SDL_FCC__AMC,
                                   ffp->vout);
    if (!vp->bmp) {
        /* SDL allocates a buffer smaller than requested if the video
         * overlay hardware is unable to support the requested size. */
        av_log(NULL, AV_LOG_FATAL,
               "Error: the video system does not support an OPAQ image\n");
        amc_free_picture(vp);
    }

    SDL_LockMutex(is->pictq.mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq.cond);
    SDL_UnlockMutex(is->pictq.mutex);
}

// like ff_ffplay.c: queue_picture()
static int amc_queue_picture(
    IJKFF_Pipenode            *node,
    SDL_AMediaCodec           *acodec,
    int                        output_buffer_index,
    SDL_AMediaCodecBufferInfo *buffer_info,
    double                     pts,
    double                     duration,
    int64_t                    pos,
    int                        serial)
{
    IJKFF_Pipenode_Opaque *opaque = node->opaque;
    FFPlayer              *ffp    = opaque->ffp;
    VideoState            *is     = ffp->is;
    Frame                 *vp;

#if defined(DEBUG_SYNC) && 0
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = ffp_frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar.num = 1;
    vp->sar.den = 1;

    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || vp->reallocate || !vp->allocated ||
        vp->width  != opaque->frame_width ||
        vp->height != opaque->frame_height ||
        !SDL_VoutOverlayAMediaCodec_isKindOf(vp->bmp)) {

        if (vp->width != opaque->frame_width || vp->height != opaque->frame_height)
            ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, opaque->frame_width, opaque->frame_height);

        vp->allocated  = 0;
        vp->reallocate = 0;
        vp->width      = opaque->frame_width;
        vp->height     = opaque->frame_height;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        amc_alloc_picture(ffp);

        if (is->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        /* get a pointer on the bitmap */
        SDL_VoutLockYUVOverlay(vp->bmp);

        /* get a pointer on the bitmap */
        if (SDL_VoutOverlayAMediaCodec_attachFrame(vp->bmp, opaque->acodec,
            output_buffer_index, buffer_info) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            exit(1);
        }
        /* update the bitmap content */
        SDL_VoutUnlockYUVOverlay(vp->bmp);

        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;
        // ALOGE("vp %lf, %lf, %d, %d", pts, duration, (int)pos, (int)serial);

        /* now we can update the picture count */
        ffp_frame_queue_push(&is->pictq);
    }
    return 0;
}

static int feedInputBuffer(JNIEnv *env, IJKFF_Pipenode *node, int64_t timeUs, int *enqueue_count)
{
    IJKFF_Pipenode_Opaque *opaque   = node->opaque;
    FFPlayer              *ffp      = opaque->ffp;
    IJKFF_Pipeline        *pipeline = opaque->pipeline;
    VideoState            *is       = ffp->is;
    Decoder               *d        = &is->viddec;
    sdl_amedia_status_t    amc_ret  = 0;
    int                    ret      = 0;

    if (enqueue_count)
        *enqueue_count = 0;

    if (d->queue->abort_request) {
        ret = 0;
        goto fail;
    }

    if (!d->packet_pending || d->queue->serial != d->pkt_serial) {
#if AMC_USE_AVBITSTREAM_FILTER
#else
        H264ConvertState convert_state = {0, 0};
#endif
        AVPacket pkt;
        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (ffp_packet_queue_get_or_buffering(ffp, d->queue, &pkt, &d->pkt_serial, &d->finished) < 0) {
                ret = -1;
                goto fail;
            }
            if (ffp_is_flush_packet(ffp, &pkt)) {
                //if (SDL_AMediaCodec_is_configured(opaque->acodec))
                    //SDL_AMediaCodec_flush(opaque->acodec);
                d->finished = 0;
                d->flushed = 1;
                d->next_pts = d->start_pts;
                d->next_pts_tb = d->start_pts_tb;
            }
        } while (ffp_is_flush_packet(ffp, &pkt) || d->queue->serial != d->pkt_serial);
        av_free_packet(&d->pkt);
        d->pkt_temp = d->pkt = pkt;
        d->packet_pending = 1;
#if AMC_USE_AVBITSTREAM_FILTER
        // d->pkt_temp->data could be allocated by av_bitstream_filter_filter
        if (d->bfsc_ret > 0) {
            if (d->bfsc_data)
                av_freep(&d->bfsc_data);
            d->bfsc_ret = 0;
        }
        d->bfsc_ret =
            av_bitstream_filter_filter(opaque->bsfc, opaque->avctx, NULL, &d->pkt_temp.data, &d->pkt_temp.size,
                                       d->pkt.data, d->pkt.size, d->pkt.flags & AV_PKT_FLAG_KEY);
        if (d->bfsc_ret > 0) {
            d->bfsc_data = d->pkt_temp.data;
        } else if (d->bfsc_ret < 0) {
            ALOGE("%s: av_bitstream_filter_filter failed\n", __func__);
            ret = -1;
            goto fail;
        }

        if (d->pkt_temp.size == d->pkt.size + opaque->avctx->extradata_size) {
            d->pkt_temp.data += opaque->avctx->extradata_size;
            d->pkt_temp.size  = d->pkt.size;
        }

        AMCTRACE("bsfc->filter(%d): %p[%d] -> %p[%d]", d->bfsc_ret, d->pkt.data, (int)d->pkt.size, d->pkt_temp.data, (int)d->pkt_temp.size);
#else
        AMCTRACE("raw [%d][%d] %02x%02x%02x%02x%02x%02x%02x%02x", (int)d->pkt_temp.size,
            (int)opaque->nal_size,
            d->pkt_temp.data[0],
            d->pkt_temp.data[1],
            d->pkt_temp.data[2],
            d->pkt_temp.data[3],
            d->pkt_temp.data[4],
            d->pkt_temp.data[5],
            d->pkt_temp.data[6],
            d->pkt_temp.data[7]);
        convert_h264_to_annexb(d->pkt_temp.data, d->pkt_temp.size, opaque->nal_size, &convert_state);
        int64_t time_stamp = d->pkt_temp.pts;
        if (!time_stamp && d->pkt_temp.dts)
            time_stamp = d->pkt_temp.dts;
        if (time_stamp > 0) {
            time_stamp = av_rescale_q(time_stamp, is->video_st->time_base, AV_TIME_BASE_Q);
        } else {
            time_stamp = 0;
        }
        AMCTRACE("input[%d][%d][%lld,%lld (%d, %d) -> %lld] %02x%02x%02x%02x%02x%02x%02x%02x", (int)d->pkt_temp.size,
            (int)opaque->nal_size,
            (int64_t)d->pkt_temp.pts,
            (int64_t)d->pkt_temp.dts,
            (int)is->video_st->time_base.num,
            (int)is->video_st->time_base.den,
            (int64_t)time_stamp,
            d->pkt_temp.data[0],
            d->pkt_temp.data[1],
            d->pkt_temp.data[2],
            d->pkt_temp.data[3],
            d->pkt_temp.data[4],
            d->pkt_temp.data[5],
            d->pkt_temp.data[6],
            d->pkt_temp.data[7]);
#endif
    }

    if (d->pkt_temp.data) {
        ssize_t  input_buffer_index = 0;
        uint8_t* input_buffer_ptr   = NULL;
        size_t   input_buffer_size  = 0;
        size_t   copy_size          = 0;
        int64_t  time_stamp         = 0;

        // reconfigure surface if surface changed
        // NULL surface cause no display
        if (ffpipeline_is_surface_need_reconfigure(pipeline)) {
            ret = reconfigureMediaCodec(env, node);
            if (ret != 0) {
                ALOGE("%s: reconfigureMediaCodec failed\n", __func__);
                ret = 0;
                goto fail;
            }
        }

        input_buffer_index = SDL_AMediaCodec_dequeueInputBuffer(opaque->acodec, timeUs);
        if (input_buffer_index < 0) {
            ALOGE("%s: SDL_AMediaCodec_dequeueInputBuffer failed(%d)\n", __func__, input_buffer_index);
            if (!SDL_AMediaCodec_isInputBuffersValid(opaque->acodec))
                ret = -1;
            else
                ret = 0;
            goto fail;
        }

        input_buffer_ptr = SDL_AMediaCodec_getInputBuffer(opaque->acodec, input_buffer_index, &input_buffer_size);
        if (!input_buffer_ptr) {
            ALOGE("%s: SDL_AMediaCodec_getInputBuffer failed\n", __func__);
            ret = -1;
            goto fail;
        }

        copy_size = FFMIN(input_buffer_size, d->pkt_temp.size);
        memcpy(input_buffer_ptr, d->pkt_temp.data, copy_size);

        time_stamp = d->pkt_temp.pts;
        if (!time_stamp && d->pkt_temp.dts)
            time_stamp = d->pkt_temp.dts;
        if (time_stamp > 0) {
            time_stamp = av_rescale_q(time_stamp, is->video_st->time_base, AV_TIME_BASE_Q);
        } else {
            time_stamp = 0;
        }
        // ALOGE("queueInputBuffer, %lld\n", time_stamp);
        amc_ret = SDL_AMediaCodec_queueInputBuffer(opaque->acodec, input_buffer_index, 0, copy_size, time_stamp, 0);
        if (amc_ret != SDL_AMEDIA_OK) {
            ALOGE("%s: SDL_AMediaCodec_getInputBuffer failed\n", __func__);
            ret = -1;
            goto fail;
        }
        // ALOGE("%s: queue %d/%d", __func__, (int)copy_size, (int)input_buffer_size);
        if (enqueue_count)
            ++*enqueue_count;

        if (input_buffer_size < 0) {
            d->packet_pending = 0;
        } else {
            d->pkt_temp.dts =
            d->pkt_temp.pts = AV_NOPTS_VALUE;
            if (d->pkt_temp.data) {
                d->pkt_temp.data += copy_size;
                d->pkt_temp.size -= copy_size;
                if (d->pkt_temp.size <= 0)
                    d->packet_pending = 0;
            } else {
                // FIXME: detect if decode finished
                // if (!got_frame) {
                    d->packet_pending = 0;
                    d->finished = d->pkt_serial;
                // }
            }
        }
    }

fail:
    return ret;
}

static int enqueue_thread_func(void *arg)
{
    JNIEnv                *env      = NULL;
    IJKFF_Pipenode        *node     = arg;
    IJKFF_Pipenode_Opaque *opaque   = node->opaque;
    FFPlayer              *ffp      = opaque->ffp;
    VideoState            *is       = ffp->is;
    int                    ret      = 0;
    int                    dequeue_count = 0;

    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("%s: SetupThreadEnv failed\n", __func__);
        return -1;
    }

    while (!is->abort_request) {
        ret = feedInputBuffer(env, node, 1000000, &dequeue_count);
        if (ret != 0) {
            goto fail;
        }
    }

fail:
    return 0;
}

static int drainOutputBuffer(JNIEnv *env, IJKFF_Pipenode *node, int64_t timeUs, int *dequeue_count)
{
    IJKFF_Pipenode_Opaque *opaque   = node->opaque;
    FFPlayer              *ffp      = opaque->ffp;
    VideoState            *is       = ffp->is;
    int                    ret      = 0;
    SDL_AMediaCodecBufferInfo bufferInfo;
    ssize_t                   output_buffer_index = 0;

    if (dequeue_count)
        *dequeue_count = 0;

    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("%s:create: SetupThreadEnv failed\n", __func__);
        return -1;
    }

    output_buffer_index = SDL_AMediaCodec_dequeueOutputBuffer(opaque->acodec, &bufferInfo, timeUs);
    if (output_buffer_index == AMEDIACODEC__INFO_OUTPUT_BUFFERS_CHANGED) {
        ALOGI("AMEDIACODEC__INFO_OUTPUT_BUFFERS_CHANGED\n");
        // continue;
    } else if (output_buffer_index == AMEDIACODEC__INFO_OUTPUT_FORMAT_CHANGED) {
        ALOGI("AMEDIACODEC__INFO_OUTPUT_FORMAT_CHANGED\n");
        SDL_AMediaFormat_deleteP(&opaque->output_aformat);
        opaque->output_aformat = SDL_AMediaCodec_getOutputFormat(opaque->acodec);
        if (opaque->output_aformat) {
            int width  = 0;
            int height = 0;

            SDL_AMediaFormat_getInt32(opaque->output_aformat, "width",  &width);
            SDL_AMediaFormat_getInt32(opaque->output_aformat, "height", &height);

            if (width > 0 && height > 0) {
                ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, width, height);
                opaque->frame_width  = width;
                opaque->frame_height = height;
            } else {
                ALOGE("AMEDIACODEC__INFO_OUTPUT_FORMAT_CHANGED: invalid width(%d), height(%d)\n", width, height);
            }
        }
        // continue;
    } else if (output_buffer_index == AMEDIACODEC__INFO_TRY_AGAIN_LATER) {
        AMCTRACE("AMEDIACODEC__INFO_TRY_AGAIN_LATER\n");
        // continue;
    } else if (output_buffer_index < 0) {
        ALOGE("%s: SDL_AMediaCodec_dequeueOutputBuffer: failed (%d)\n", __func__, (int)output_buffer_index);
        ret = -1;
        goto fail;
    } else if (output_buffer_index >= 0) {
        double     pts;
        double     duration;
        AVRational tb = is->video_st->time_base;
        AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

        if (dequeue_count)
            ++*dequeue_count;

        duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
        int64_t amc_pts = av_rescale_q(bufferInfo.presentationTimeUs, AV_TIME_BASE_Q, is->video_st->time_base);
        pts = amc_pts < 0 ? NAN : amc_pts * av_q2d(tb);
        // ALOGE("got_frame: %lld -> %lf", bufferInfo.presentationTimeUs, pts);
        ret = amc_queue_picture(node, opaque->acodec, output_buffer_index, &bufferInfo, pts, duration, 0, is->viddec.pkt_serial);
    }

    ret = 0;
fail:
    return ret;
}

static void func_destroy(IJKFF_Pipenode *node)
{
    if (!node || !node->opaque)
        return;

    IJKFF_Pipenode_Opaque *opaque = node->opaque;

    SDL_AMediaCodec_deleteP(&opaque->acodec);
    SDL_AMediaFormat_deleteP(&opaque->input_aformat);
    SDL_AMediaFormat_deleteP(&opaque->output_aformat);

    av_freep(&opaque->orig_extradata);

    if (opaque->bsfc) {
        av_bitstream_filter_close(opaque->bsfc);
        opaque->bsfc = NULL;
    }

    JNIEnv *env = NULL;
    if (JNI_OK == SDL_JNI_SetupThreadEnv(&env)) {
        SDL_JNI_DeleteGlobalRefP(env, &opaque->jsurface);
    }
}

static int func_run_sync(IJKFF_Pipenode *node)
{
    JNIEnv                *env      = NULL;
    IJKFF_Pipenode_Opaque *opaque   = node->opaque;
    FFPlayer              *ffp      = opaque->ffp;
    VideoState            *is       = ffp->is;
    int                    ret      = 0;
    int                    dequeue_count = 0;

    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("%s: SetupThreadEnv failed\n", __func__);
        return -1;
    }

    // SDL_Delay(5000);

    opaque->acodec = SDL_AMediaCodecJava_createDecoderByType(env, opaque->codec_mime);
    if (!opaque->acodec) {
        ALOGE("%s: SDL_AMediaCodecJava_createDecoderByType(%s) failed\n", __func__, opaque->codec_mime);
        ret = -1;
        goto fail;
    }

    ffpipeline_set_surface_need_reconfigure(opaque->pipeline, true);
    ret = reconfigureMediaCodec(env, node);
    if (ret != 0) {
        ALOGE("%s: reconfigureMediaCodec failed\n", __func__);
        goto fail;
    }

    drainOutputBuffer(env, node, 0, &dequeue_count);

    opaque->enqueue_thread = SDL_CreateThreadEx(&opaque->_enqueue_thread, enqueue_thread_func, node, "amediacodec_input_thread");
    if (!opaque->enqueue_thread) {
        ALOGE("%s: SDL_CreateThreadEx failed\n", __func__);
        ret = -1;
        goto fail;
    }

    while (!is->abort_request) {
        ret = drainOutputBuffer(env, node, 1000000, &dequeue_count);
        if (ret != 0) {
            ret = -1;
            goto fail;
        }
    }

fail:
    if (opaque->acodec)
        SDL_AMediaCodec_stop(opaque->acodec);
    SDL_WaitThread(opaque->enqueue_thread, NULL);
    return ret;
#if 0
fallback_to_ffplay:
    ALOGW("fallback to ffplay decoder\n");
    return ffp_video_thread(opaque->ffp);
#endif
}

IJKFF_Pipenode *ffpipenode_create_video_decoder_from_android_mediacodec(FFPlayer *ffp, IJKFF_Pipeline *pipeline, SDL_Vout *vout)
{
    ALOGD("ffpipenode_create_video_decoder_from_android_mediacodec()\n");
    if (SDL_Android_GetApiLevel() < IJK_API_16_JELLY_BEAN)
        return NULL;

    if (!ffp || !ffp->is)
        return NULL;

    IJKFF_Pipenode *node = ffpipenode_alloc(sizeof(IJKFF_Pipenode_Opaque));
    if (!node)
        return node;

    VideoState            *is         = ffp->is;
    IJKFF_Pipenode_Opaque *opaque     = node->opaque;
    JNIEnv                *env        = NULL;

    node->func_destroy  = func_destroy;
    node->func_run_sync = func_run_sync;
    opaque->pipeline    = pipeline;
    opaque->ffp         = ffp;
    opaque->decoder     = &is->viddec;
    opaque->weak_vout   = vout;

    opaque->avctx = opaque->decoder->avctx;
    switch (opaque->avctx->codec_id) {
    case AV_CODEC_ID_H264:
        opaque->codec_mime = SDL_AMIME_VIDEO_AVC;
        break;
    default:
        ALOGE("%s:create: not H264\n", __func__);
        goto fail;
    }

    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("%s:create: SetupThreadEnv failed\n", __func__);
        goto fail;
    }

    opaque->acodec = SDL_AMediaCodecJava_createDecoderByType(env, opaque->codec_mime);
    if (!opaque->acodec) {
        ALOGE("%s:open_video_decoder: SDL_AMediaCodecJava_createDecoderByType(%s) failed\n", __func__, opaque->codec_mime);
        goto fail;
    }
    assert(opaque->weak_vout);
    SDL_VoutAndroid_setAMediaCodec(opaque->weak_vout, opaque->acodec);

    ALOGI("AMediaFormat: %s, %dx%d\n", opaque->codec_mime, opaque->avctx->width, opaque->avctx->height);
    opaque->input_aformat = SDL_AMediaFormatJava_createVideoFormat(env, opaque->codec_mime, opaque->avctx->width, opaque->avctx->height);
    if (opaque->avctx->extradata && opaque->avctx->extradata_size > 0) {
        if (opaque->avctx->codec_id == AV_CODEC_ID_H264 && opaque->avctx->extradata[0] == 1) {
#if AMC_USE_AVBITSTREAM_FILTER
            opaque->bsfc = av_bitstream_filter_init("h264_mp4toannexb");
            if (!opaque->bsfc) {
                ALOGE("Cannot open the h264_mp4toannexb BSF!\n");
                goto fail;
            }

            opaque->orig_extradata_size = opaque->avctx->extradata_size;
            opaque->orig_extradata = (uint8_t*) av_mallocz(opaque->avctx->extradata_size +
                                                      FF_INPUT_BUFFER_PADDING_SIZE);
            if (!opaque->orig_extradata) {
                goto fail;
            }
            memcpy(opaque->orig_extradata, opaque->avctx->extradata, opaque->avctx->extradata_size);
            for(int i = 0; i < opaque->avctx->extradata_size; i+=4) {
                ALOGE("csd-0[%d]: %02x%02x%02x%02x\n", opaque->avctx->extradata_size, (int)opaque->avctx->extradata[i+0], (int)opaque->avctx->extradata[i+1], (int)opaque->avctx->extradata[i+2], (int)opaque->avctx->extradata[i+3]);
            }
            SDL_AMediaFormat_setBuffer(opaque->input_aformat, "csd-0", opaque->avctx->extradata, opaque->avctx->extradata_size);
#else
            size_t   sps_pps_size   = 0;
            size_t   convert_size   = opaque->avctx->extradata_size + 20;
            uint8_t *convert_buffer = (uint8_t *)calloc(1, convert_size);
            if (!convert_buffer) {
                ALOGE("%s:sps_pps_buffer: alloc failed\n", __func__);
                goto fail;
            }
            if (0 != convert_sps_pps(opaque->avctx->extradata, opaque->avctx->extradata_size,
                                     convert_buffer, convert_size,
                                     &sps_pps_size, &opaque->nal_size)) {
                ALOGE("%s:convert_sps_pps: failed\n", __func__);
                goto fail;
            }
            SDL_AMediaFormat_setBuffer(opaque->input_aformat, "csd-0", convert_buffer, sps_pps_size);
            for(int i = 0; i < sps_pps_size; i+=4) {
                ALOGE("csd-0[%d]: %02x%02x%02x%02x\n", sps_pps_size, (int)convert_buffer[i+0], (int)convert_buffer[i+1], (int)convert_buffer[i+2], (int)convert_buffer[i+3]);
            }
            free(convert_buffer);
#endif
        } else {
            // Codec specific data
            // SDL_AMediaFormat_setBuffer(opaque->aformat, "csd-0", opaque->avctx->extradata, opaque->avctx->extradata_size);
            ALOGE("csd-0: naked\n");
        }
    } else {
        ALOGE("no buffer(%d)\n", opaque->avctx->extradata_size);
    }

    return node;
fail:
    ffpipenode_free_p(&node);
    return NULL;
}
