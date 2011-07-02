/*
 * PCM audio output driver
 * Copyright (C) 2011 Rudolf Polzer <divVerent@xonotic.org>
 * NOTE: this file is partially based on ao_pcm.c by Atmosfear
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include "mpcommon.h"
#include "libavutil/common.h"
#include "fmt-conversion.h"
#include "libaf/af_format.h"
#include "libaf/reorder_ch.h"
#include "talloc.h"
#include "audio_out.h"
#include "mp_msg.h"

#include "encode_lavc.h"

static const char *sample_padding_signed = "\x00\x00\x00\x00";
static const char *sample_padding_u8     = "\x80";
static const char *sample_padding_float  = "\x00\x00\x00\x00";

struct priv {
    uint8_t *buffer;
    size_t buffer_size;
    AVStream *stream;
    int pcmhack;
    int aframesize;
    int aframecount;
    int offset;
    int64_t savepts;
    int framecount;
    int64_t lastpts;
    int sample_size;
    const void *sample_padding;
    int restptsvalid;
    double restpts;
};

// open & setup audio device
static int init(struct ao *ao, char *params){
    struct priv *ac = talloc_zero(ao, struct priv);
    const enum AVSampleFormat *sampleformat;
    AVCodec *codec;

    if (!encode_lavc_available()) {
        mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: the option -o (output file) must be specified\n");
        return -1;
    }

    if (ac->stream) {
        mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: rejecting reinitialization\n");
        return -1;
    }

    ac->stream = encode_lavc_alloc_stream(AVMEDIA_TYPE_AUDIO);
    codec = encode_lavc_get_codec(ac->stream);

    // ac->stream->time_base.num = 1;
    // ac->stream->time_base.den = ao->samplerate;
    // doing this breaks mpeg2ts in ffmpeg
    // which doesn't properly force the time base to be 90000
    // furthermore, ffmpeg.c doesn't do this either and works

    ac->stream->codec->time_base.num = 1;
    ac->stream->codec->time_base.den = ao->samplerate;

    ac->stream->codec->sample_rate = ao->samplerate;
    ac->stream->codec->channels = ao->channels;

    ac->stream->codec->sample_fmt = SAMPLE_FMT_NONE;

    {
        // first check if the selected format is somewhere in the list of
        // supported formats by the codec
        for (sampleformat = codec->sample_fmts; sampleformat && *sampleformat != SAMPLE_FMT_NONE; ++sampleformat) {
            switch (*sampleformat) {
                case SAMPLE_FMT_U8:
                    if (ao->format == AF_FORMAT_U8)
                        goto out_search;
                    break;
                case SAMPLE_FMT_S16:
                    if (ao->format == AF_FORMAT_S16_BE)
                        goto out_search;
                    if (ao->format == AF_FORMAT_S16_LE)
                        goto out_search;
                    break;
                case SAMPLE_FMT_S32:
                    if (ao->format == AF_FORMAT_S32_BE)
                        goto out_search;
                    if (ao->format == AF_FORMAT_S32_LE)
                        goto out_search;
                    break;
                case SAMPLE_FMT_FLT:
                    if (ao->format == AF_FORMAT_FLOAT_BE)
                        goto out_search;
                    if (ao->format == AF_FORMAT_FLOAT_LE)
                        goto out_search;
                    break;
                default:
                    break;
            }
        }
out_search:
        ;
    }

    if (!sampleformat || *sampleformat == SAMPLE_FMT_NONE) {
        // if the selected format is not supported, we have to pick the first
        // one we CAN support
        // note: not needing to select endianness here, as the switch() below
        // does that anyway for us
        for (sampleformat = codec->sample_fmts; sampleformat && *sampleformat != SAMPLE_FMT_NONE; ++sampleformat) {
            switch (*sampleformat) {
                case SAMPLE_FMT_U8:
                    ao->format = AF_FORMAT_U8;
                    goto out_takefirst;
                case SAMPLE_FMT_S16:
                    ao->format = AF_FORMAT_S16_NE;
                    goto out_takefirst;
                case SAMPLE_FMT_S32:
                    ao->format = AF_FORMAT_S32_NE;
                    goto out_takefirst;
                case SAMPLE_FMT_FLT:
                    ao->format = AF_FORMAT_FLOAT_NE;
                    goto out_takefirst;
                default:
                    break;
            }
        }
out_takefirst:
        ;
    }

    switch (ao->format) {
        // now that we have chosen a format, set up the fields for it, boldly
        // switching endianness if needed (mplayer code will convert for us
        // anyway, but ffmpeg always expects native endianness)
        case AF_FORMAT_U8:
            ac->stream->codec->sample_fmt = SAMPLE_FMT_U8;
            ac->sample_size = 1;
            ac->sample_padding = sample_padding_u8;
            ao->format = AF_FORMAT_U8;
            break;
        default:
        case AF_FORMAT_S16_BE:
        case AF_FORMAT_S16_LE:
            ac->stream->codec->sample_fmt = SAMPLE_FMT_S16;
            ac->sample_size = 2;
            ac->sample_padding = sample_padding_signed;
            ao->format = AF_FORMAT_S16_NE;
            break;
        case AF_FORMAT_S32_BE:
        case AF_FORMAT_S32_LE:
            ac->stream->codec->sample_fmt = SAMPLE_FMT_S32;
            ac->sample_size = 4;
            ac->sample_padding = sample_padding_signed;
            ao->format = AF_FORMAT_S32_NE;
            break;
        case AF_FORMAT_FLOAT_BE:
        case AF_FORMAT_FLOAT_LE:
            ac->stream->codec->sample_fmt = SAMPLE_FMT_FLT;
            ac->sample_size = 4;
            ac->sample_padding = sample_padding_float;
            ao->format = AF_FORMAT_FLOAT_NE;
            break;
    }

    ac->stream->codec->bits_per_raw_sample = ac->sample_size * 8;

    switch (ao->channels) {
        case 1:
            ac->stream->codec->channel_layout = AV_CH_LAYOUT_MONO;
            break;
        case 2:
            ac->stream->codec->channel_layout = AV_CH_LAYOUT_STEREO;
            break;
        /* someone please check if these are what mplayer normally assumes
        case 3:
            ac->stream->codec->channel_layout = AV_CH_LAYOUT_SURROUND;
            break;
        case 4:
            ac->stream->codec->channel_layout = AV_CH_LAYOUT_2_2;
            break;
        */
        case 5:
            ac->stream->codec->channel_layout = AV_CH_LAYOUT_5POINT0;
            break;
        case 6:
            ac->stream->codec->channel_layout = AV_CH_LAYOUT_5POINT1;
            break;
        case 8:
            ac->stream->codec->channel_layout = AV_CH_LAYOUT_7POINT1;
            break;
        default:
            mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: unknown channel layout; hoping for the best\n");
            break;
    }

    if (encode_lavc_open_codec(ac->stream) < 0) {
        mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: unable to open encoder\n");
        return -1;
    }

    ac->pcmhack = 0;
    if(ac->stream->codec->frame_size <= 1) {
        ac->pcmhack = av_get_bits_per_sample(ac->stream->codec->codec_id) / 8;
    }

    if (ac->pcmhack) {
        ac->aframesize = 16384; // "enough"
        ac->buffer_size = ac->aframesize * ac->pcmhack * ao->channels * 2 + 200;
    } else {
        ac->aframesize = ac->stream->codec->frame_size;
        ac->buffer_size = ac->aframesize * ac->sample_size * ao->channels * 2 + 200;
    }
    if (ac->buffer_size < FF_MIN_BUFFER_SIZE)
        ac->buffer_size = FF_MIN_BUFFER_SIZE;
    ac->buffer = talloc_size(ac, ac->buffer_size);
    ac->restpts = MP_NOPTS_VALUE;

    // enough frames for at least 0.25 seconds
    ac->framecount = ceil(ao->samplerate * 0.25 / ac->aframesize);
    // but at least one!
    ac->framecount = max(ac->framecount, 1);

    ac->savepts = MP_NOPTS_VALUE;
    ac->lastpts = MP_NOPTS_VALUE;
    ac->offset = ac->stream->codec->sample_rate * encode_lavc_getoffset(ac->stream);

    //fill_ao_data:
    ao->outburst = ac->aframesize * ac->sample_size * ao->channels * ac->framecount;
    ao->buffersize = ao->outburst * 2;
    ao->bps = ao->channels * ao->samplerate * ac->sample_size;
    ao->untimed = true;
    ao->priv = ac;

    return 0;
}

static void fill_with_padding(void *buf, int cnt, int sz, const void *padding)
{
    int i;
    if (sz == 1) {
        memset(buf, cnt, *(char *)padding);
        return;
    }
    for (i = 0; i < cnt; ++i)
        memcpy((char *) buf + i * sz, padding, sz);
}

// close audio device
static int encode(struct ao *ao, int ptsvalid, double apts, void *data);
static void uninit(struct ao *ao, bool cut_audio){
    struct priv *ac = ao->priv;
    if (ac->buffer) {
        if (ao->buffer.len > 0) {
            void *paddingbuf = talloc_size(ao, ac->aframesize * ao->channels * ac->sample_size);
            memcpy(paddingbuf, ao->buffer.start, ao->buffer.len);
            fill_with_padding((char *) paddingbuf + ao->buffer.len, (ac->aframesize * ao->channels * ac->sample_size - ao->buffer.len) / ac->sample_size, ac->sample_size, ac->sample_padding);
            encode(ao,
                    ac->restptsvalid,
                    ac->restpts,
                    paddingbuf);
            ac->restpts += ac->aframesize / (double) ao->samplerate;
            talloc_free(paddingbuf);
        }
        while (encode(ao, 
                    ac->restptsvalid,
                    ac->restpts,
                    NULL) > 0);
    }

    ao->priv = NULL;
}

// return: how many bytes can be played without blocking
static int get_space(struct ao *ao){
    return ao->outburst;
}

static int encode(struct ao *ao, int ptsvalid, double apts, void *data) // must be exactly ac->aframesize amount of data
{
    struct priv *ac = ao->priv;
    int size;
    double realapts = ac->aframecount++ * (double) ac->aframesize / (double) ao->samplerate;

    if (data && (ao->channels == 5 || ao->channels == 6 || ao->channels == 8)) {
        reorder_channel_nch(data, AF_CHANNEL_LAYOUT_MPLAYER_DEFAULT,
                            AF_CHANNEL_LAYOUT_LAVC_DEFAULT,
                            ao->channels,
                            ac->aframesize * ao->channels, ac->sample_size);
    }

    if(data && ptsvalid)
        encode_lavc_settimesync(realapts - apts, (double) ac->aframesize / (double) ao->samplerate);

    if (ac->pcmhack && data)
        size = avcodec_encode_audio(ac->stream->codec, ac->buffer, ac->aframesize * ac->pcmhack * ao->channels, data);
    else
        size = avcodec_encode_audio(ac->stream->codec, ac->buffer, ac->buffer_size, data);

    mp_msg(MSGT_AO, MSGL_DBG2, "ao-lavc: got pts %f (playback time: %f); out size: %d\n", apts, realapts, size);

    encode_lavc_write_stats(ac->stream);

    if(ac->savepts == MP_NOPTS_VALUE) {
        ac->savepts = floor(realapts * (double) ac->stream->time_base.den / (double) ac->stream->time_base.num + 0.5);
    }

    if (size < 0) {
        mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: error encoding\n");
    }

    if (size > 0) {
        AVPacket packet;
        av_init_packet(&packet);
        packet.stream_index = ac->stream->index;
        packet.data = ac->buffer;
        packet.size = size;

        /* TODO: enable this code once ffmpeg.c adds something similar; till then, do the same as ffmpeg.c: mark ALL audio frames as key frames
           if(ac->stream->codec->coded_frame && ac->stream->codec->coded_frame->keyframe)
           packet.flags |= AV_PKT_FLAG_KEY;
           else if(!ac->stream->codec->coded_frame)
         */
        packet.flags |= AV_PKT_FLAG_KEY;

        if (ac->stream->codec->coded_frame && ac->stream->codec->coded_frame->pts != AV_NOPTS_VALUE) {
            packet.pts = av_rescale_q(ac->stream->codec->coded_frame->pts, ac->stream->codec->time_base, ac->stream->time_base);
        } else {
            packet.pts = ac->savepts;
        }
        ac->savepts = MP_NOPTS_VALUE;

        if (encode_lavc_testflag(ENCODE_LAVC_FLAG_COPYTS)) {
            // we are NOT fixing video pts to match audio playback time
            // so we MUST set video-compatible pts!
            packet.pts = floor(packet.pts + (apts - realapts) * ac->stream->time_base.den / ac->stream->time_base.num + 0.5);
        }

        if (packet.pts != AV_NOPTS_VALUE) {
            if (ac->lastpts != MP_NOPTS_VALUE && packet.pts <= ac->lastpts) {
                // this indicates broken video (video pts failing to increase fast enough to match audio)
                mp_msg(MSGT_AO, MSGL_WARN, "ao-lavc: audio pts went backwards (%d <- %d), autofixed\n", (int)packet.pts, (int)ac->lastpts);
                packet.pts = ac->lastpts + 1;
            }
            ac->lastpts = packet.pts;
        }

        if (encode_lavc_write_frame(&packet) < 0) {
            mp_msg(MSGT_AO, MSGL_ERR, "ao-lavc: error writing at %f %f/%f\n", realapts, (double) ac->stream->time_base.num, (double) ac->stream->time_base.den);
            return -1;
        }
    }

    return size;
}

// plays 'len' bytes of 'data'
// it should round it down to outburst*n
// return: number of bytes played
static int play(struct ao *ao, void* data,int len,int flags){
    struct priv *ac = ao->priv;
    int bufpos = 0;
    int ptsvalid;
    int64_t ptsoffset;
    void *paddingbuf = NULL;

    len /= ac->sample_size * ao->channels;

    if (!encode_lavc_start())
        return 0;

    if (encode_lavc_testflag(ENCODE_LAVC_FLAG_COPYTS)) {
        // we do not send time sync data to the video side, but we always need the exact pts, even if zero
        ptsvalid = 1;
        ptsoffset = ac->offset;
    } else {
        ptsvalid = (ao->apts > 0); // FIXME for some reason I sometimes get invalid apts == 0 when seeking... don't initialize time sync from that
        if (ac->offset < 0) {
            if (ac->offset <= -len) {
                // skip whole frame
                ac->offset += len;
                return len * ac->sample_size * ao->channels;
            } else {
                // skip part of this frame, buffer/encode the rest
                bufpos += -ac->offset;
                ac->offset = 0;
            }
        } else if (ac->offset > 0) {
            // make a temporary buffer, filled with zeroes at the start (don't worry, only happens once)

            paddingbuf = talloc_size(ac, ac->sample_size * ao->channels * (ac->offset + len));
            fill_with_padding(paddingbuf, ac->offset, ac->sample_size, ac->sample_padding);
            data = (char *) paddingbuf + ac->sample_size * ao->channels * ac->offset;
            bufpos -= ac->offset; // yes, negative!
            ac->offset = 0;

            // now adjust the bufpos so the final value of bufpos is positive!
            {
                /*
                int cnt = (len - bufpos) / ac->aframesize;
                int finalbufpos = bufpos + cnt * ac->aframesize;
                */
                int finalbufpos = len - (len - bufpos) % ac->aframesize;
                if (finalbufpos < 0) {
                    mp_msg(MSGT_AO, MSGL_WARN, "ao-lavc: cannot attain the exact requested audio sync; shifting by %d frames\n", -finalbufpos);
                    bufpos -= finalbufpos;
                }
            }
        }
        ptsoffset = 0;
    }

    while (len - bufpos >= ac->aframesize) {
        encode(ao,
                ptsvalid,
                ao->apts + (bufpos + ptsoffset) / (double) ao->samplerate + encode_lavc_getoffset(ac->stream),
                (char *) data + ac->sample_size * bufpos * ao->channels);
        bufpos += ac->aframesize;
    }

    // this is for the case uninit() gets called
    ac->restpts = ao->apts + (bufpos + ptsoffset) / (double) ao->samplerate + encode_lavc_getoffset(ac->stream);
    ac->restptsvalid = ptsvalid;

    if(paddingbuf)
        talloc_free(paddingbuf);

    return bufpos * ac->sample_size * ao->channels;
}

const struct ao_driver audio_out_lavc = {
    .is_new = true,
    .info = &(const struct ao_info) {
        "audio encoding using libavcodec",
        "lavc",
        "Rudolf Polzer <divVerent@xonotic.org>",
        ""
    },
    .init      = init,
    .uninit    = uninit,
    .get_space = get_space,
    .play      = play,
};
