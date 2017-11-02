/*
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

#include "mp_msg.h"
#include "libavutil/avutil.h"
#include <libavutil/pixdesc.h>
#include "libmpcodecs/img_format.h"
#include "fmt-conversion.h"

static const struct {
    int fmt;
    enum AVPixelFormat pix_fmt;
} conversion_map[] = {
    {IMGFMT_ARGB, AV_PIX_FMT_ARGB},
    {IMGFMT_BGRA, AV_PIX_FMT_BGRA},
    {IMGFMT_BGR24, AV_PIX_FMT_BGR24},
    {IMGFMT_BGR16BE, AV_PIX_FMT_RGB565BE},
    {IMGFMT_BGR16LE, AV_PIX_FMT_RGB565LE},
    {IMGFMT_BGR15BE, AV_PIX_FMT_RGB555BE},
    {IMGFMT_BGR15LE, AV_PIX_FMT_RGB555LE},
    {IMGFMT_BGR12BE, AV_PIX_FMT_RGB444BE},
    {IMGFMT_BGR12LE, AV_PIX_FMT_RGB444LE},
    {IMGFMT_BGR8,  AV_PIX_FMT_RGB8},
    {IMGFMT_BGR4,  AV_PIX_FMT_RGB4},
    {IMGFMT_BGR1,  AV_PIX_FMT_MONOBLACK},
    {IMGFMT_RGB1,  AV_PIX_FMT_MONOBLACK},
    {IMGFMT_RG4B,  AV_PIX_FMT_BGR4_BYTE},
    {IMGFMT_BG4B,  AV_PIX_FMT_RGB4_BYTE},
    {IMGFMT_RGB48LE, AV_PIX_FMT_RGB48LE},
    {IMGFMT_RGB48BE, AV_PIX_FMT_RGB48BE},
    {IMGFMT_ABGR, AV_PIX_FMT_ABGR},
    {IMGFMT_RGBA, AV_PIX_FMT_RGBA},
    {IMGFMT_RGB24, AV_PIX_FMT_RGB24},
    {IMGFMT_RGB16BE, AV_PIX_FMT_BGR565BE},
    {IMGFMT_RGB16LE, AV_PIX_FMT_BGR565LE},
    {IMGFMT_RGB15BE, AV_PIX_FMT_BGR555BE},
    {IMGFMT_RGB15LE, AV_PIX_FMT_BGR555LE},
    {IMGFMT_RGB12BE, AV_PIX_FMT_BGR444BE},
    {IMGFMT_RGB12LE, AV_PIX_FMT_BGR444LE},
    {IMGFMT_RGB8,  AV_PIX_FMT_BGR8},
    {IMGFMT_RGB4,  AV_PIX_FMT_BGR4},
    {IMGFMT_BGR8,  AV_PIX_FMT_PAL8},
    {IMGFMT_GBRP,  AV_PIX_FMT_GBRP},
    {IMGFMT_GBRP9, AV_PIX_FMT_GBRP9},
    {IMGFMT_GBRP10, AV_PIX_FMT_GBRP10},
    {IMGFMT_YUY2,  AV_PIX_FMT_YUYV422},
    {IMGFMT_UYVY,  AV_PIX_FMT_UYVY422},
    {IMGFMT_NV12,  AV_PIX_FMT_NV12},
    {IMGFMT_NV21,  AV_PIX_FMT_NV21},
    {IMGFMT_Y800,  AV_PIX_FMT_GRAY8},
    {IMGFMT_Y8,    AV_PIX_FMT_GRAY8},
    {IMGFMT_YVU9,  AV_PIX_FMT_YUV410P},
    {IMGFMT_IF09,  AV_PIX_FMT_YUV410P},
    {IMGFMT_YV12,  AV_PIX_FMT_YUV420P},
    {IMGFMT_I420,  AV_PIX_FMT_YUV420P},
    {IMGFMT_IYUV,  AV_PIX_FMT_YUV420P},
    {IMGFMT_411P,  AV_PIX_FMT_YUV411P},
    {IMGFMT_422P,  AV_PIX_FMT_YUV422P},
    {IMGFMT_444P,  AV_PIX_FMT_YUV444P},
    {IMGFMT_440P,  AV_PIX_FMT_YUV440P},

    {IMGFMT_420A,  AV_PIX_FMT_YUVA420P},

    {IMGFMT_420P16_LE,  AV_PIX_FMT_YUV420P16LE},
    {IMGFMT_420P16_BE,  AV_PIX_FMT_YUV420P16BE},
    {IMGFMT_420P9_LE,   AV_PIX_FMT_YUV420P9LE},
    {IMGFMT_420P9_BE,   AV_PIX_FMT_YUV420P9BE},
    {IMGFMT_420P10_LE,  AV_PIX_FMT_YUV420P10LE},
    {IMGFMT_420P10_BE,  AV_PIX_FMT_YUV420P10BE},
    {IMGFMT_422P10_LE,  AV_PIX_FMT_YUV422P10LE},
    {IMGFMT_422P10_BE,  AV_PIX_FMT_YUV422P10BE},
    {IMGFMT_444P9_BE ,  AV_PIX_FMT_YUV444P9BE},
    {IMGFMT_444P9_LE ,  AV_PIX_FMT_YUV444P9LE},
    {IMGFMT_444P10_BE,  AV_PIX_FMT_YUV444P10BE},
    {IMGFMT_444P10_LE,  AV_PIX_FMT_YUV444P10LE},
    {IMGFMT_422P16_LE,  AV_PIX_FMT_YUV422P16LE},
    {IMGFMT_422P16_BE,  AV_PIX_FMT_YUV422P16BE},
    {IMGFMT_422P9_LE,   AV_PIX_FMT_YUV422P9LE},
    {IMGFMT_422P9_BE,   AV_PIX_FMT_YUV422P9BE},
    {IMGFMT_444P16_LE,  AV_PIX_FMT_YUV444P16LE},
    {IMGFMT_444P16_BE,  AV_PIX_FMT_YUV444P16BE},

    // YUVJ are YUV formats that use the full Y range and not just
    // 16 - 235 (see colorspaces.txt).
    // Currently they are all treated the same way.
    {IMGFMT_YV12,  AV_PIX_FMT_YUVJ420P},
    {IMGFMT_422P,  AV_PIX_FMT_YUVJ422P},
    {IMGFMT_444P,  AV_PIX_FMT_YUVJ444P},
    {IMGFMT_440P,  AV_PIX_FMT_YUVJ440P},

    {IMGFMT_VDPAU_MPEG1,     AV_PIX_FMT_VDPAU_MPEG1},
    {IMGFMT_VDPAU_MPEG2,     AV_PIX_FMT_VDPAU_MPEG2},
    {IMGFMT_VDPAU_H264,      AV_PIX_FMT_VDPAU_H264},
    {IMGFMT_VDPAU_WMV3,      AV_PIX_FMT_VDPAU_WMV3},
    {IMGFMT_VDPAU_VC1,       AV_PIX_FMT_VDPAU_VC1},
    {IMGFMT_VDPAU_MPEG4,     AV_PIX_FMT_VDPAU_MPEG4},
    {0,                      AV_PIX_FMT_NONE}
};

enum AVPixelFormat imgfmt2pixfmt(int fmt)
{
    int i;
    enum AVPixelFormat pix_fmt;
    for (i = 0; conversion_map[i].fmt; i++)
        if (conversion_map[i].fmt == fmt)
            break;
    pix_fmt = conversion_map[i].pix_fmt;
    if (pix_fmt == AV_PIX_FMT_NONE)
        mp_msg(MSGT_GLOBAL, MSGL_ERR, "Unsupported format %s\n", vo_format_name(fmt));
    return pix_fmt;
}

int pixfmt2imgfmt(enum AVPixelFormat pix_fmt)
{
    int i;
    for (i = 0; conversion_map[i].pix_fmt != AV_PIX_FMT_NONE; i++)
        if (conversion_map[i].pix_fmt == pix_fmt)
            break;
    int fmt = conversion_map[i].fmt;
    if (!fmt) {
        const char *fmtname = av_get_pix_fmt_name(pix_fmt);
        mp_msg(MSGT_GLOBAL, MSGL_ERR, "Unsupported AVPixelFormat %s (%d)\n",
               fmtname ? fmtname : "INVALID", pix_fmt);
    }
    return fmt;
}
