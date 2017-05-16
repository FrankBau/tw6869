/*
 * Copyright 2015 www.starterkit.ru <info@starterkit.ru>
 *
 * Based on:
 * tw686x common header file
 * Copyright 2009-10 liran <jlee@techwellinc.com.cn> [Techwell China]
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __TW6869_H
#define __TW6869_H

#define PCI_VENDOR_ID_TECHWELL  0x1797
#define PCI_DEVICE_ID_6869      0x6869

#define TW_FRAME_MAX      3
#define TW_APAGE_MAX      16
#define TW_CH_MAX         8
#define TW_VIN_MAX        1
#define TW_PAGE_SIZE      4096

#define TW_VID            0x00FF
#define TW_AID            0xFF00
#define TW_CH0to3         0x0F
#define TW_CH4to7         0xF0

#define ID2ID(did)        ((did) & 0xF)
#define ID2CH(did)        ((did) & 0x7)
#define ID2SC(did)        ((did) & 0x3)

#define TW_DMA_ON         1
#define TW_DMA_OFF        2
#define TW_DMA_RST        3
#define TW_DMA_DESC_UNIT  512
#define TW_FIFO_ERR(did)  ((BIT(ID2ID(did)) & TW_VID) ? ((BIT(ID2CH(did)) << 24) | (BIT(ID2CH(did)) << 16)) : 0)
#define TW_PARS_ERR(did)  ((BIT(ID2ID(did)) & TW_VID) ? ((BIT(ID2CH(did)) << 8)  | BIT(ID2CH(did))) : 0)

#define TW_STD_NTSC_M     0
#define TW_STD_PAL        1
#define TW_STD_SECAM      2
#define TW_STD_NTSC_443   3
#define TW_STD_PAL_M      4
#define TW_STD_PAL_CN     5
#define TW_STD_PAL_60     6
#define TW_STD_NOT_VALID  7

#define TW_FMT_UYVY       0
#define TW_FMT_RGB565     5
#define TW_FMT_YUYV       6

/**
 * Register definitions
 */
#define R32_INT_STATUS              0x000                        /* 0x00 */
#define R32_PB_STATUS               0x004                        /* 0x01 */
#define R32_DMA_CMD                 0x008                        /* 0x02 */
#define R32_FIFO_STATUS             0x00C                        /* 0x03 */
#define R32_VIDEO_CHANNEL_ID        0x010                        /* 0x04 */
#define R32_VIDEO_PARSER_STATUS     0x014                        /* 0x05 */
#define R32_SYS_SOFT_RST            0x018                        /* 0x06 */
#define R32_DMA_CHANNEL_ENABLE      0x028                        /* 0x0a */
#define R32_DMA_CONFIG              0x02C                        /* 0x0b */
#define R32_DMA_TIMER_INTERVAL      0x030                        /* 0x0c */
#define R32_DMA_CHANNEL_TIMEOUT     0x034                        /* 0x0d */
#define R32_DMA_CHANNEL_CONFIG(did) (0x040 + ID2CH(did) * 0x04)  /* 0x10 */
#define R32_VIDEO_CONTROL1          0x0A8                        /* 0x2A */
#define R32_VIDEO_CONTROL2          0x0AC                        /* 0x2B */
#define R32_AUDIO_CONTROL1          0x0B0                        /* 0x2C */
#define R32_AUDIO_CONTROL2          0x0B4                        /* 0x2D */
#define R32_PHASE_REF               0x0B8                        /* 0x2E */
#define R32_VIDEO_FIELD_CTRL(did)   (0x0E4 + ID2CH(did) * 0x04)  /* 0x39 */
#define R32_DMA_P_ADDR(did)         ((BIT(ID2ID(did)) & TW_VID) ? (0x200 + ID2CH(did) * 0x20) : (0x060 + ID2CH(did) * 0x08))
#define R32_DMA_B_ADDR(did)         ((BIT(ID2ID(did)) & TW_VID) ? (0x208 + ID2CH(did) * 0x20) : (0x064 + ID2CH(did) * 0x08))
#define R32_DMA_WHP(did)            (0x204 + ID2CH(did) * 0x20)  /* 0x81 */
#define R32_F2_DMA_P_ADDR(did)      (0x210 + ID2CH(did) * 0x20)  /* 0x84 */
#define R32_F2_DMA_B_ADDR(did)      (0x218 + ID2CH(did) * 0x20)  /* 0x86 */
#define R32_F2_DMA_WHP(did)         (0x214 + ID2CH(did) * 0x20)  /* 0x85 */


#define CSR_REG                     0x3F4                        /* 0xFD */
#define EP_REG_ADDR                 0x3F8                        /* 0xFE */
#define EP_REG_DATA                 0x3FC                        /* 0xFF */

#define VERTICAL_CTRL               0x63c                        /* 0x18F */

#define R8_REG(addr,did)         (4*(addr) + ((BIT(ID2CH(did)) & TW_CH0to3) ? 0 : 0x400) + ID2SC(did) * 0x40)

/* 0x100, 0x200 */
#define R8_VIDEO_STATUS(did)         (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x400 : 0x800) + ID2SC(did) * 0x40)
/* 0x101, 0x201 */
#define R8_BRIGHT_CTRL(did)         (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x404 : 0x804) + ID2SC(did) * 0x40)
/* 0x102, 0x202 */
#define R8_CONTRAST_CTRL(did)       (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x408 : 0x808) + ID2SC(did) * 0x40)
/* 0x104, 0x204 */
#define R8_SAT_U_CTRL(did)          (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x410 : 0x810) + ID2SC(did) * 0x40)
/* 0x105, 0x205 */
#define R8_SAT_V_CTRL(did)          (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x414 : 0x814) + ID2SC(did) * 0x40)
/* 0x106, 0x206 */
#define R8_HUE_CTRL(did)            (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x418 : 0x818) + ID2SC(did) * 0x40)
/* 0x108, 0x208 */
#define R8_VDELAY(did)              (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x420 : 0x820) + ID2SC(did) * 0x40)
/* 0x10D, 0x20D */
#define R8_CHIP_STATUS_II(did)      (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x434 : 0x834) + ID2SC(did) * 0x40)
/* 0x10E, 0x20E */
#define R8_STANDARD_SEL(did)        (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x438 : 0x838) + ID2SC(did) * 0x40)
/* 0x10F, 0x20F */
#define R8_STANDARD_REC(did)        (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x43C : 0x83C) + ID2SC(did) * 0x40)
/* 0x180, 0x280 */
#define R8_AVSRST(did)              ((BIT(ID2CH(did)) & TW_CH0to3) ? 0x600 : 0xA00)
/* 0x18F, 0x28F */
#define R8_VERTICAL_CONTROL1(did)   ((BIT(ID2CH(did)) & TW_CH0to3) ? 0x63C : 0xA3C)
/* 0x194, 0x294 */
#define R8_MISC_CONTROL1(did)       ((BIT(ID2CH(did)) & TW_CH0to3) ? 0x650 : 0xA50)
/* 0x196, 0x296 */
#define R8_MISC_CONTROL2(did)       ((BIT(ID2CH(did)) & TW_CH0to3) ? 0x658 : 0xA58)
/* 0x1D0, 0x2D0 */
#define R8_AIGAIN_CTRL(did)         (((BIT(ID2CH(did)) & TW_CH0to3) ? 0x740 : 0xB40) + ID2SC(did) * 0x04)

#endif /* __TW6869_H */
