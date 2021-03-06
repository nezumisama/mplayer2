/*
 * audio filter for runtime AC-3 encoding with libavcodec.
 *
 * Copyright (C) 2007 Ulion <ulion A gmail P com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavresample/avresample.h>

#include "config.h"
#include "af.h"
#include "reorder_ch.h"


#define AC3_MAX_CHANNELS 6
#define AC3_MAX_CODED_FRAME_SIZE 3840
#define AC3_FRAME_SIZE (6  * 256)
const uint16_t ac3_bitrate_tab[19] = {
    32, 40, 48, 56, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 448, 512, 576, 640
};

// Data for specific instances of this filter
typedef struct af_ac3enc_s {
    AVAudioResampleContext *avr;
    uint8_t *resample_buf[AC3_MAX_CHANNELS];
    int linesize;
    AVFrame *frame;
    struct AVCodec        *lavc_acodec;
    struct AVCodecContext *lavc_actx;
    int add_iec61937_header;
    int bit_rate;
    int pending_data_size;
    char *pending_data;
    int pending_len;
    int expect_len;
    int min_channel_num;
    int in_sampleformat;
} af_ac3enc_t;

// Initialization and runtime control
static int control(struct af_instance_s *af, int cmd, void *arg)
{
    af_ac3enc_t *s  = (af_ac3enc_t *)af->setup;
    af_data_t *data = (af_data_t *)arg;
    int i, bit_rate, test_output_res;
    static const int default_bit_rate[AC3_MAX_CHANNELS+1] = \
        {0, 96000, 192000, 256000, 384000, 448000, 448000};

    switch (cmd){
    case AF_CONTROL_REINIT:
        if (AF_FORMAT_IS_AC3(data->format) || data->nch < s->min_channel_num)
            return AF_DETACH;

        af->data->format = s->in_sampleformat;
        af->data->bps = af_fmt2bits(s->in_sampleformat) / 8;
        if (data->rate == 48000 || data->rate == 44100 || data->rate == 32000)
            af->data->rate = data->rate;
        else
            af->data->rate = 48000;
        if (data->nch > AC3_MAX_CHANNELS)
            af->data->nch = AC3_MAX_CHANNELS;
        else
            af->data->nch = data->nch;
        test_output_res = af_test_output(af, data);

        s->pending_len = 0;
        s->expect_len = AC3_FRAME_SIZE * data->nch * af->data->bps;
        assert(s->expect_len <= s->pending_data_size);
        if (s->add_iec61937_header)
            af->mul = (double)AC3_FRAME_SIZE * 2 * 2 / s->expect_len;
        else
            af->mul = (double)AC3_MAX_CODED_FRAME_SIZE / s->expect_len;

        mp_msg(MSGT_AFILTER, MSGL_DBG2, "af_lavcac3enc reinit: %d, %d, %f, %d.\n",
               data->nch, data->rate, af->mul, s->expect_len);

        bit_rate = s->bit_rate ? s->bit_rate : default_bit_rate[af->data->nch];

        if (s->lavc_actx->channels != af->data->nch ||
                s->lavc_actx->sample_rate != af->data->rate ||
                s->lavc_actx->bit_rate != bit_rate) {

            avcodec_close(s->lavc_actx);

            if (s->avr) {
                uint64_t ch_layout =
                    av_get_default_channel_layout(af->data->nch);
                enum AVSampleFormat in_sample_fmt =
                    av_get_packed_sample_fmt(s->lavc_actx->sample_fmt);
                int ret;

                avresample_close(s->avr);

                if (af->data->nch != s->lavc_actx->channels) {
                    av_freep(&s->resample_buf[0]);
                    ret = av_samples_alloc(s->resample_buf, &s->linesize,
                                           af->data->nch, AC3_FRAME_SIZE,
                                           s->lavc_actx->sample_fmt, 0);
                    if (ret < 0) {
                        uint8_t error[128];
                        av_strerror(ret, error, sizeof(error));
                        mp_msg(MSGT_AFILTER, MSGL_ERR, "Error allocating "
                               "resample buffer: %s\n", error);
                        return AF_ERROR;
                    }
                }

                av_opt_set_int(s->avr, "in_channel_layout",   ch_layout, 0);
                av_opt_set_int(s->avr, "out_channel_layout",  ch_layout, 0);
                av_opt_set_int(s->avr, "in_sample_rate",    af->data->rate, 0);
                av_opt_set_int(s->avr, "out_sample_rate",   af->data->rate, 0);
                av_opt_set_int(s->avr, "in_sample_fmt",     in_sample_fmt, 0);
                av_opt_set_int(s->avr, "out_sample_fmt",    s->lavc_actx->sample_fmt, 0);

                if ((ret = avresample_open(s->avr)) < 0) {
                    uint8_t error[128];
                    av_strerror(ret, error, sizeof(error));
                    mp_msg(MSGT_AFILTER, MSGL_ERR, "Error configuring "
                           "libavresample: %s\n", error);
                    return AF_ERROR;
                }
            }

            // Put sample parameters
            s->lavc_actx->channels = af->data->nch;
            s->lavc_actx->sample_rate = af->data->rate;
            s->lavc_actx->bit_rate = bit_rate;

            if (avcodec_open2(s->lavc_actx, s->lavc_acodec, NULL) < 0) {
                mp_tmsg(MSGT_AFILTER, MSGL_ERR, "Couldn't open codec %s, br=%d.\n", "ac3", bit_rate);
                return AF_ERROR;
            }
        }
        if (s->lavc_actx->frame_size != AC3_FRAME_SIZE) {
            mp_msg(MSGT_AFILTER, MSGL_ERR, "lavcac3enc: unexpected ac3 "
                   "encoder frame size %d\n", s->lavc_actx->frame_size);
            return AF_ERROR;
        }
        af->data->format = AF_FORMAT_AC3_BE;
        af->data->bps = 2;
        af->data->nch = 2;
        return test_output_res;
    case AF_CONTROL_COMMAND_LINE:
        mp_msg(MSGT_AFILTER, MSGL_DBG2, "af_lavcac3enc cmdline: %s.\n", (char*)arg);
        s->bit_rate = 0;
        s->min_channel_num = 0;
        s->add_iec61937_header = 0;
        sscanf((char*)arg,"%d:%d:%d", &s->add_iec61937_header, &s->bit_rate,
               &s->min_channel_num);
        if (s->bit_rate < 1000)
            s->bit_rate *= 1000;
        if (s->bit_rate) {
            for (i = 0; i < 19; ++i)
                if (ac3_bitrate_tab[i] * 1000 == s->bit_rate)
                    break;
            if (i >= 19) {
                mp_msg(MSGT_AFILTER, MSGL_WARN, "af_lavcac3enc unable set unsupported "
                       "bitrate %d, use default bitrate (check manpage to see "
                       "supported bitrates).\n", s->bit_rate);
                s->bit_rate = 0;
            }
        }
        if (s->min_channel_num == 0)
            s->min_channel_num = 5;
        mp_msg(MSGT_AFILTER, MSGL_V, "af_lavcac3enc config spdif:%d, bitrate:%d, "
               "minchnum:%d.\n", s->add_iec61937_header, s->bit_rate,
               s->min_channel_num);
        return AF_OK;
    }
    return AF_UNKNOWN;
}

// Deallocate memory
static void uninit(struct af_instance_s* af)
{
    if (af->data)
        free(af->data->audio);
    free(af->data);
    if (af->setup) {
        af_ac3enc_t *s = af->setup;
        af->setup = NULL;
        if(s->lavc_actx) {
            avcodec_close(s->lavc_actx);
            av_free(s->lavc_actx);
        }
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54, 28, 0)
        av_frame_free(&s->frame);
#else
        av_freep(&s->frame);
#endif
        avresample_free(&s->avr);
        av_freep(&s->resample_buf[0]);
        free(s->pending_data);
        free(s);
    }
}

static int encode_data(af_ac3enc_t *s, uint8_t *src, uint8_t *dst, int dst_len)
{
    AVPacket pkt;
    uint8_t error[128];
    int total_samples = AC3_FRAME_SIZE * s->lavc_actx->channels;
    int bps = av_get_bytes_per_sample(s->lavc_actx->sample_fmt);
    int ret, got_frame;

    if (s->lavc_actx->channels >= 5)
        reorder_channel_nch(src, AF_CHANNEL_LAYOUT_MPLAYER_DEFAULT,
                            AF_CHANNEL_LAYOUT_LAVC_DEFAULT,
                            s->lavc_actx->channels,
                            total_samples, bps);

    s->frame->nb_samples  = AC3_FRAME_SIZE;
    s->frame->data[0]     = src;
    s->frame->linesize[0] = total_samples * bps;

    if (s->avr) {
        ret = avresample_convert(s->avr, s->resample_buf, s->linesize,
                                 AC3_FRAME_SIZE, &src, total_samples * bps,
                                 AC3_FRAME_SIZE);
        if (ret < 0) {
            av_strerror(ret, error, sizeof(error));
            mp_msg(MSGT_AFILTER, MSGL_ERR, "Error converting audio sample "
                   "format: %s\n", error);
            return AF_ERROR;
        } else if (ret != AC3_FRAME_SIZE) {
            mp_msg(MSGT_AFILTER, MSGL_ERR, "Not enough converted data.\n");
            return -1;
        }

        memcpy(s->frame->data, s->resample_buf, sizeof(s->resample_buf));
        s->frame->linesize[0] = s->linesize;
    }

    av_init_packet(&pkt);
    pkt.data = dst;
    pkt.size = dst_len;

    ret = avcodec_encode_audio2(s->lavc_actx, &pkt, s->frame, &got_frame);
    if (ret < 0) {
        av_strerror(ret, error, sizeof(error));
        mp_msg(MSGT_AFILTER, MSGL_ERR, "Error encoding audio: %s\n", error);
        return ret;
    }
    return got_frame ? pkt.size : 0;
}

// Filter data through filter
static af_data_t* play(struct af_instance_s* af, af_data_t* data)
{
    af_ac3enc_t *s = af->setup;
    af_data_t *c = data;    // Current working data
    af_data_t *l;
    int len, left, outsize = 0, destsize;
    char *buf, *src, *dest;
    int max_output_len;
    int frame_num = (data->len + s->pending_len) / s->expect_len;

    if (s->add_iec61937_header)
        max_output_len = AC3_FRAME_SIZE * 2 * 2 * frame_num;
    else
        max_output_len = AC3_MAX_CODED_FRAME_SIZE * frame_num;

    if (af->data->len < max_output_len) {
        mp_msg(MSGT_AFILTER, MSGL_V, "[libaf] Reallocating memory in module %s, "
               "old len = %i, new len = %i\n", af->info->name, af->data->len,
                max_output_len);
        free(af->data->audio);
        af->data->audio = malloc(max_output_len);
        if (!af->data->audio) {
            mp_msg(MSGT_AFILTER, MSGL_FATAL, "[libaf] Could not allocate memory \n");
            return NULL;
        }
        af->data->len = max_output_len;
    }

    l = af->data;           // Local data
    buf = (char *)l->audio;
    src = (char *)c->audio;
    left = c->len;


    while (left > 0) {
        if (left + s->pending_len < s->expect_len) {
            memcpy(s->pending_data + s->pending_len, src, left);
            src += left;
            s->pending_len += left;
            left = 0;
            break;
        }

        dest = s->add_iec61937_header ? buf + 8 : buf;
        destsize = (char *)l->audio + l->len - buf;

        if (s->pending_len) {
            int needs = s->expect_len - s->pending_len;
            if (needs > 0) {
                memcpy(s->pending_data + s->pending_len, src, needs);
                src += needs;
                left -= needs;
            }

            len = encode_data(s, s->pending_data, dest, destsize);
            s->pending_len = 0;
        }
        else {
            len = encode_data(s, src, dest, destsize);
            src += s->expect_len;
            left -= s->expect_len;
        }
        if (len <= 0)
            return NULL;

        mp_msg(MSGT_AFILTER, MSGL_DBG2, "avcodec_encode_audio got %d, pending %d.\n",
               len, s->pending_len);

        if (s->add_iec61937_header) {
            int bsmod = dest[5] & 0x7;

            AV_WB16(buf,     0xF872);   // iec 61937 syncword 1
            AV_WB16(buf + 2, 0x4E1F);   // iec 61937 syncword 2
            buf[4] = bsmod;             // bsmod
            buf[5] = 0x01;              // data-type ac3
            AV_WB16(buf + 6, len << 3); // number of bits in payload

            memset(buf + 8 + len, 0, AC3_FRAME_SIZE * 2 * 2 - 8 - len);
            len = AC3_FRAME_SIZE * 2 * 2;
        }

        outsize += len;
        buf += len;
    }
    c->audio = l->audio;
    c->nch   = 2;
    c->bps   = 2;
    c->len   = outsize;
    mp_msg(MSGT_AFILTER, MSGL_DBG2, "play return size %d, pending %d\n",
           outsize, s->pending_len);
    return c;
}

static int af_open(af_instance_t* af){

    af_ac3enc_t *s = calloc(1,sizeof(af_ac3enc_t));
    af->control=control;
    af->uninit=uninit;
    af->play=play;
    af->mul=1;
    af->data=calloc(1,sizeof(af_data_t));
    af->setup=s;

    s->lavc_acodec = avcodec_find_encoder_by_name("ac3");
    if (!s->lavc_acodec) {
        mp_tmsg(MSGT_AFILTER, MSGL_ERR, "Audio LAVC, couldn't find encoder for codec %s.\n", "ac3");
        return AF_ERROR;
    }

    s->lavc_actx = avcodec_alloc_context3(s->lavc_acodec);
    if (!s->lavc_actx) {
        mp_tmsg(MSGT_AFILTER, MSGL_ERR, "Audio LAVC, couldn't allocate context!\n");
        return AF_ERROR;
    }
    s->frame = av_frame_alloc();
    if (!s->frame)
        return AF_ERROR;
    const enum AVSampleFormat *fmts = s->lavc_acodec->sample_fmts;
    for (int i = 0; ; i++) {
        if (fmts[i] == AV_SAMPLE_FMT_NONE) {
            mp_msg(MSGT_AFILTER, MSGL_ERR, "Audio LAVC, encoder doesn't "
                   "support expected sample formats!\n");
            return AF_ERROR;
        }
        enum AVSampleFormat fmt_packed = av_get_packed_sample_fmt(fmts[i]);
        if (fmt_packed == AV_SAMPLE_FMT_S16) {
            s->in_sampleformat = AF_FORMAT_S16_NE;
            s->lavc_actx->sample_fmt = fmts[i];
            break;
        } else if (fmt_packed == AV_SAMPLE_FMT_FLT) {
            s->in_sampleformat = AF_FORMAT_FLOAT_NE;
            s->lavc_actx->sample_fmt = fmts[i];
            break;
        }
    }
    if (av_sample_fmt_is_planar(s->lavc_actx->sample_fmt)) {
        s->avr = avresample_alloc_context();
        if (!s->avr)
            abort();
    }
    char buf[100];
    mp_msg(MSGT_AFILTER, MSGL_V, "[af_lavcac3enc]: in sample format: %s\n",
           af_fmt2str(s->in_sampleformat, buf, 100));
    s->pending_data_size = AF_NCH * AC3_FRAME_SIZE *
        af_fmt2bits(s->in_sampleformat) / 8;
    s->pending_data = malloc(s->pending_data_size);

    return AF_OK;
}

af_info_t af_info_lavcac3enc = {
    "runtime encode to ac3 using libavcodec",
    "lavcac3enc",
    "Ulion",
    "",
    AF_FLAGS_REENTRANT,
    af_open
};
