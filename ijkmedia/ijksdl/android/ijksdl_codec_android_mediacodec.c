/*****************************************************************************
 * ijksdl_codec_android_mediacodec.c
 *****************************************************************************
 *
 * copyright (c) 2014 Zhang Rui <bbcallen@gmail.com>
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

#include "ijksdl_codec_android_mediacodec.h"
#include <assert.h>

// FIXME: release SDL_AMediaCodec
sdl_amedia_status_t SDL_AMediaCodec_delete(SDL_AMediaCodec* acodec)
{
    if (!acodec)
        return SDL_AMEDIA_OK;

    assert(acodec->func_delete);
    return acodec->func_delete(acodec);
}

sdl_amedia_status_t SDL_AMediaCodec_deleteP(SDL_AMediaCodec** acodec)
{
    if (!acodec)
        return SDL_AMEDIA_OK;
    return SDL_AMediaCodec_delete(*acodec);
}

sdl_amedia_status_t SDL_AMediaCodec_configure(
    SDL_AMediaCodec* acodec,
    const SDL_AMediaFormat* aformat,
    ANativeWindow* surface,
    SDL_AMediaCrypto *crypto,
    uint32_t flags)
{
    assert(acodec->func_configure);
    sdl_amedia_status_t ret = acodec->func_configure(acodec, aformat, surface, crypto, flags);
    acodec->is_configured = true;
    return ret;
}

sdl_amedia_status_t SDL_AMediaCodec_configure_surface(
    JNIEnv*env,
    SDL_AMediaCodec* acodec,
    const SDL_AMediaFormat* aformat,
    jobject android_surface,
    SDL_AMediaCrypto *crypto,
    uint32_t flags)
{
    assert(acodec->func_configure_surface);
    sdl_amedia_status_t ret = acodec->func_configure_surface(env, acodec, aformat, android_surface, crypto, flags);
    acodec->is_configured = true;
    return ret;
}

bool SDL_AMediaCodec_is_configured(SDL_AMediaCodec *acodec)
{
    return acodec->is_configured;
}

sdl_amedia_status_t SDL_AMediaCodec_start(SDL_AMediaCodec* acodec)
{
    assert(acodec->func_start);
    return acodec->func_start(acodec);
}

sdl_amedia_status_t SDL_AMediaCodec_stop(SDL_AMediaCodec* acodec)
{
    assert(acodec->func_stop);
    return acodec->func_stop(acodec);
}

sdl_amedia_status_t SDL_AMediaCodec_flush(SDL_AMediaCodec* acodec)
{
    assert(acodec->func_flush);
    return acodec->func_flush(acodec);
}

uint8_t* SDL_AMediaCodec_getInputBuffer(SDL_AMediaCodec* acodec, size_t idx, size_t *out_size)
{
    assert(acodec->func_getInputBuffer);
    return acodec->func_getInputBuffer(acodec, idx, out_size);
}

uint8_t* SDL_AMediaCodec_getOutputBuffer(SDL_AMediaCodec* acodec, size_t idx, size_t *out_size)
{
    assert(acodec->func_getOutputBuffer);
    return acodec->func_getOutputBuffer(acodec, idx, out_size);
}

ssize_t SDL_AMediaCodec_dequeueInputBuffer(SDL_AMediaCodec* acodec, int64_t timeoutUs)
{
    assert(acodec->func_dequeueInputBuffer);
    return acodec->func_dequeueInputBuffer(acodec, timeoutUs);
}

sdl_amedia_status_t SDL_AMediaCodec_queueInputBuffer(SDL_AMediaCodec* acodec, size_t idx, off_t offset, size_t size, uint64_t time, uint32_t flags)
{
    assert(acodec->func_queueInputBuffer);
    return acodec->func_queueInputBuffer(acodec, idx, offset, size, time, flags);
}

ssize_t SDL_AMediaCodec_dequeueOutputBuffer(SDL_AMediaCodec* acodec, SDL_AMediaCodecBufferInfo *info, int64_t timeoutUs)
{
    assert(acodec->func_dequeueOutputBuffer);
    return acodec->func_dequeueOutputBuffer(acodec, info, timeoutUs);
}

SDL_AMediaFormat* SDL_AMediaCodec_getOutputFormat(SDL_AMediaCodec* acodec)
{
    assert(acodec->func_getOutputFormat);
    return acodec->func_getOutputFormat(acodec);
}

sdl_amedia_status_t SDL_AMediaCodec_releaseOutputBuffer(SDL_AMediaCodec* acodec, size_t idx, bool render)
{
    assert(acodec->func_releaseOutputBuffer);
    return acodec->func_releaseOutputBuffer(acodec, idx, render);
}

bool SDL_AMediaCodec_isInputBuffersValid(SDL_AMediaCodec* acodec)
{
    assert(acodec->func_isInputBuffersValid);
    return acodec->func_isInputBuffersValid(acodec);
}
