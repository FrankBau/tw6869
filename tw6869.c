/*
 * Copyright 2015 www.starterkit.ru <info@starterkit.ru>
 *
 * Based on:
 * Driver for Intersil|Techwell TW6869 based DVR cards
 * (c) 2011-12 liran <jli11@intersil.com> [Intersil|Techwell China]
 *
 * V4L2 PCI Skeleton Driver
 * Copyright 2014 Cisco Systems, Inc. and/or its affiliates.
 * All rights reserved.
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

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-contig.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/control.h>

#include "tw6869.h"

MODULE_DESCRIPTION("tw6869/65 media bridge driver");
MODULE_AUTHOR("starterkit <info@starterkit.ru>");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.3.1");


#define CHECK_V_PB  /* drop frames on pb failures */
#undef IMPL_AUDIO   /* not used/checked */
#undef IRQ_DMA_STOP
#define LAST_ENABLE
#undef EARLY_DISABLE

#define TRACE_SIZE     (64*1024)
#define TW_NAME "tw6869"
#define TWINFO(FMT, ...)   DP_PRINT("I[" TW_NAME ":%s:%d] " FMT, __func__, __LINE__,##__VA_ARGS__)
#define TWNOTICE(FMT, ...) DP_PRINT("N[" TW_NAME ":%s:%d] " FMT, __func__, __LINE__,##__VA_ARGS__)
#define TWWARN(FMT, ...)   { DP_PRINT("W[" TW_NAME ":%s:%d] " FMT, __func__, __LINE__,##__VA_ARGS__); pr_warning("[" TW_NAME ":%s:%d] " FMT, __func__, __LINE__,##__VA_ARGS__); }
#define TWERR(FMT, ...)   printk( KERN_ERR "[" TW_NAME ":%s:%d] " FMT, __func__, __LINE__,##__VA_ARGS__)


#if !TRACE_SIZE
#define DP_PRINT(FMT,...)
#else
#define DP_PRINT(FMT,...) \
	if (ramdp.buf) {\
		uint32_t __len; \
		unsigned long __flags;\
		spin_lock_irqsave(&ramdp.lock, __flags); \
		mb(); \
		do { \
			struct timeval tv; do_gettimeofday(&tv); \
			__len = snprintf(ramdp.buf + ramdp.tracePos, TRACE_SIZE - ramdp.tracePos, "%u:%ld.%06ld:%c:" FMT,\
					ramdp.traceCount, tv.tv_sec, tv.tv_usec, (in_interrupt() ? 'I' : ((in_atomic() ? 'A' : 'N'))),\
##__VA_ARGS__); \
			if (__len + ramdp.tracePos >= TRACE_SIZE) {\
				ramdp.written += (TRACE_SIZE - ramdp.tracePos);\
				ramdp.tracePos = 0;\
			} else { \
				ramdp.tracePos += __len;\
				ramdp.written += __len;\
			} \
		} while (0 == ramdp.tracePos && __len < TRACE_SIZE); \
		ramdp.traceCount += 1;\
		ramdp.pos = ramdp.buf + ramdp.tracePos;\
		mb(); \
		spin_unlock_irqrestore(&ramdp.lock, __flags); \
	} else { \
		pr_notice("[" TW_NAME ":%s:%d] " FMT, __func__, __LINE__,##__VA_ARGS__); \
	}

struct ramdp_handle_t {
	uint8_t buf[TRACE_SIZE];
	uint32_t len;
	spinlock_t lock;

	volatile uint8_t * pos;
	volatile uint64_t written;

	struct proc_dir_entry *proc_entry_cat;

	uint32_t tracePos;                /**< next free trace buffer position */
	uint32_t traceCount;              /**< trace counter */
}

static ramdp;
#endif



static const struct pci_device_id tw6869_pci_tbl[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_TECHWELL, PCI_DEVICE_ID_6869)},
	{ 0, }
};

struct tw6869_buf {
	struct vb2_buffer vb;
	struct list_head list;
	dma_addr_t dma;
};

#ifdef IMPL_AUDIO
/**
 * struct tw6869_ach - instance of one audio channel
 * @dev: parent device
 * @ss: audio channel (pcm substream)
 * @id: DMA id
 * @p_buf: DMA P-buffer
 * @b_buf: DMA B-buffer
 * @pb: P-buffer | B-buffer ping-pong state
 * @ptr: PCM buffer pointer
 * @buf: split an contiguous buffer into chunks
 * @buf_list: chunk list
 * @lock: spinlock controlling access to channel
 */
struct tw6869_ach {
	struct tw6869_dev *dev;
	struct snd_pcm_substream *ss;
	unsigned int id;
	struct tw6869_buf *p_buf;
	struct tw6869_buf *b_buf;
	unsigned int pb;

	dma_addr_t ptr;

	struct tw6869_buf buf[TW_APAGE_MAX];
	struct list_head buf_list;
	spinlock_t lock;
};
#endif /* IMPL_AUDIO */

/**
 * struct tw6869_vch - instance of one video channel
 * @dev: parent device
 * @vdev: video channel (v4l2 video device)
 * @id: DMA id
 * @p_buf: DMA P-buffer
 * @b_buf: DMA B-buffer
 * @pb: P-buffer | B-buffer ping-pong state
 * @hdl: handler for control framework
 * @format: pixel format
 * @std: video standard (e.g. PAL/NTSC)
 * @input: input line for video signal
 * @mlock: the main serialization lock
 * @queue: queue maintained by videobuf2 layer
 * @buff_list: list of buffer in use
 * @lock: spinlock controlling access to channel
 * @sequence: sequence number of acquired buffer
 * @dcount: number of dropped frames
 * @fps: current frame rate
 */
struct tw6869_vch {
	struct tw6869_dev *dev;
	struct video_device vdev;
	unsigned int id;
	struct tw6869_buf *p_buf;
	struct tw6869_buf *b_buf;

	struct v4l2_ctrl_handler hdl;
	struct v4l2_pix_format format;
	v4l2_std_id std;
	unsigned int input;

	struct mutex mlock;
	struct vb2_queue queue;
	struct list_head buf_list;
	spinlock_t lock;

	unsigned int sequence;
	unsigned int dcount;
	unsigned int lcount;
	unsigned int fps;
  ktime_t frame_ts;
};

/**
 * struct tw6869_dev - instance of device
 * @pdev: PCI device
 * @mmio: hardware base address
 * @rlock: spinlock controlling access to registers
 * @rlock: spinlock controlling access to queues
 * @ch_max: channels used
 * @v4l2_dev: device registered in v4l2 layer
 * @alloc_ctx: context for videobuf2
 * @vch: array of video channel instance
 * @snd_card: device registered in ALSA layer
 * @ach: array of audio channel instance
 */
struct tw6869_dev {
	struct pci_dev *pdev;
	unsigned char __iomem *mmio;
	spinlock_t rlock;
	unsigned int ch_max;

	struct v4l2_device v4l2_dev;
	struct vb2_alloc_ctx *alloc_ctx;
	struct tw6869_vch vch[TW_CH_MAX];

#ifdef IMPL_AUDIO
	struct snd_card *snd_card;
	struct tw6869_ach ach[TW_CH_MAX];
#endif /* IMPL_AUDIO */

	/// DMA smart control
	unsigned int videoCap_ID;	      /* DMA channels that are active by V4L request */
	unsigned     dma_enable;	      /* DMA enable register */
	unsigned     dma_disable;	      /* DMA to disable channels */
	unsigned     dma_error_mask;	      /* DMA mask to ign errors after reset */
	unsigned     dma_ok;	          /* DMA mask to ign errors after reset */
	ktime_t      dma_error_ts[TW_CH_MAX]; /* DMA error timestamp per channel */
	struct timer_list dma_resync;         /* DMA resync timer */
  unsigned pb_sts;
#ifdef LAST_ENABLE
	unsigned int dma_last_enable;
#endif /* LAST_ENABLE */
	unsigned int reg;                     /**< active register for reg show */

#if TRACE_SIZE
	struct ramdp_handle_t ramdp;          /**< handle for /proc ram buffer debug interface */
#endif
};

/**********************************************************************/

static inline void tw_write(struct tw6869_dev *dev, unsigned int reg, unsigned int val)
{
	//printk("tw_write reg=0x%04x val=0x%08x\n", reg, val );
	iowrite32(val, dev->mmio + reg);
}

static inline unsigned int tw_read(struct tw6869_dev *dev, unsigned int reg)
{
	unsigned int val = ioread32(dev->mmio + reg);
	return val;
}

static inline void tw_write_mask(struct tw6869_dev *dev, unsigned int reg, unsigned int val, unsigned int mask)
{
	unsigned int v = tw_read(dev, reg);

	v = (v & ~mask) | (val & mask);
	tw_write(dev, reg, v);
}

static inline void tw_clear(struct tw6869_dev *dev, unsigned int reg, unsigned int val)
{
	tw_write_mask(dev, reg, 0, val);
}

static inline void tw_set(struct tw6869_dev *dev, unsigned int reg, unsigned int val)
{
	tw_write_mask(dev, reg, val, val);
}


/**********************************************************************/

static ssize_t tw_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct v4l2_device *v4l2_dev = dev_get_drvdata(dev);
	struct tw6869_dev *tw_dev = container_of(v4l2_dev, struct tw6869_dev, v4l2_dev);
	unsigned int val = tw_read(tw_dev, 4*tw_dev->reg);
	pr_info("tw_reg_show %03X: %08X (%p)\n", tw_dev->reg, val, tw_dev);
	return sprintf(buf, "%03X: %08X\n", tw_dev->reg, val);
}

static ssize_t tw_reg_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned val;
	struct v4l2_device *v4l2_dev = dev_get_drvdata(dev);
	struct tw6869_dev *tw_dev = container_of(v4l2_dev, struct tw6869_dev, v4l2_dev);
	int ret = sscanf(buf, "%x=%x", &tw_dev->reg, &val);
	if (0 == ret || tw_dev->reg >= 0x400) {
		tw_dev->reg = 0;
		pr_info("tw_reg register:%03X (illegal register range '%s'", tw_dev->reg, buf);
	} else if (1 == ret) {
		pr_info("tw_reg register:%03X active\n", tw_dev->reg);
	} else if (2 == ret) {
		pr_info("tw_reg register:%03X set to %08X\n", tw_dev->reg, val);
		tw_write(tw_dev, 4*tw_dev->reg, val);
	}
	return count;
}

static DEVICE_ATTR(tw_reg, 0644, tw_reg_show, tw_reg_store);

#if TRACE_SIZE
static int ramdp_show(struct seq_file *m, void *v)
{
	uint8_t* read_pos;
	int wrapped;
	unsigned long flags;

	spin_lock_irqsave(&ramdp.lock, flags);
	wrapped = (ramdp.written > ramdp.len);
	/* buf-----v           v------- buf_offs (start of file / end of file)
	 *         +-------------------------------------+
	 *         |           | <---- first_half ---->  |
	 *         +-------------------------------------+
	 */

	read_pos = (uint8_t*) ramdp.pos;
	if (!wrapped) {
		unsigned len = read_pos - ramdp.buf;
		spin_unlock_irqrestore(&ramdp.lock, flags);
		seq_printf(m, "--- log %u bytes ---\n%*s", len, len, ramdp.buf);
	} else {
		unsigned buf_offs    = read_pos - ramdp.buf;
		unsigned first_half  = ramdp.len - buf_offs;
		spin_unlock_irqrestore(&ramdp.lock, flags);
		seq_printf(m, "--- log %u/%u bytes ...\n%*s%*s", first_half, buf_offs, first_half, read_pos, buf_offs, ramdp.buf);
	}
	return 0;
}


static int ramdp_open(struct inode *inode, struct file *file)
{
	return single_open(file, ramdp_show, NULL);
}

static const struct file_operations ramdp_fops = {
	.owner	= THIS_MODULE,
	.open	= ramdp_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release= single_release,
};

#endif /* TRACE_SIZE */

/**********************************************************************/

// Bug?!: audio resets video DMA channels (origin code) (RSR)
static void tw6869_id_dma_cmd(struct tw6869_dev *dev,
				unsigned int did,
				unsigned int cmd)
{
	unsigned int id = ID2ID(did);
  unsigned reg;

	switch (cmd) {
		case TW_DMA_ON:
			dev->videoCap_ID |= BIT(id);
			TWINFO("DMA %u ON\n", id);
			tw_set(dev, R32_DMA_CHANNEL_ENABLE, BIT(id));
			tw_set(dev, R32_DMA_CMD, BIT(31) | BIT(id));
			dev->dma_enable = tw_read(dev, R32_DMA_CHANNEL_ENABLE);
			dev->dma_error_mask |= BIT(id);
			tw_read(dev, R32_DMA_CMD);
			break;
		case TW_DMA_OFF:
			dev->videoCap_ID &= ~BIT(id);
			TWINFO("DMA %u OFF\n", id);
			tw_clear(dev, R32_DMA_CHANNEL_ENABLE, BIT(id));
			tw_clear(dev, R32_DMA_CMD, BIT(id));
			if (!(dev->dma_enable = tw_read(dev, R32_DMA_CHANNEL_ENABLE))) {
			  TWINFO("DMA disabled\n");
				tw_write(dev, R32_DMA_CMD, 0);
			}
			tw_read(dev, R32_DMA_CMD);
			break;
		case TW_DMA_RST:
			if (tw_read(dev, R32_DMA_CHANNEL_ENABLE) &
					tw_read(dev, R32_DMA_CMD) & BIT(id)) {
				TWINFO("DMA %u RST\n", id);
				tw_clear(dev, R32_DMA_CHANNEL_ENABLE, BIT(id));
				tw_clear(dev, R32_DMA_CMD, BIT(id));

				tw_read(dev, R32_DMA_CHANNEL_ENABLE);
				tw_read(dev, R32_DMA_CMD);
				reg = tw_read(dev, R32_DMA_P_ADDR(id));
				tw_write(dev, R32_DMA_P_ADDR(id), reg);
				reg = tw_read(dev, R32_DMA_B_ADDR(id));
				tw_write(dev, R32_DMA_B_ADDR(id), reg);

				tw_set(dev, R32_DMA_CHANNEL_ENABLE, BIT(id));
				tw_set(dev, R32_DMA_CMD, BIT(31) | BIT(id));
				dev->dma_enable = tw_read(dev, R32_DMA_CHANNEL_ENABLE);
				dev->dma_error_mask |= BIT(id);
				tw_read(dev, R32_DMA_CMD);
			} else {
				TWWARN("DMA %u spurious RST\n", id);
			}
			break;
		default:
			TWWARN("DMA %u unknown cmd %u\n", id, cmd);
	}
}

static unsigned tw6869_virq(struct tw6869_dev *dev,
				unsigned int id,
				unsigned int pb
    )
{
	unsigned long flags;
	struct tw6869_vch *vch = &dev->vch[ID2CH(id)];
	struct tw6869_buf *done = NULL;
	struct tw6869_buf *next = NULL;
	ktime_t ktime = ktime_get();
  unsigned addr;

	spin_lock_irqsave(&vch->lock, flags);
	if (!vb2_is_streaming(&vch->queue) || !vch->p_buf || !vch->b_buf) {
		spin_unlock_irqrestore(&vch->lock, flags);
		return 0;
	}

#ifdef CHECK_V_PB
	if ((vch->sequence & 1) != (pb ? 1 : 0)) {
		spin_unlock_irqrestore(&vch->lock, flags);
		++vch->dcount;
		TWINFO("vin/%u bad frame %u pb:%d last frame for %llu mysec (%llu) dropped (%u)\n", id+1, vch->sequence, pb, ktime_us_delta(ktime, vch->frame_ts), ktime_to_ms(ktime), vch->dcount);
		return 1;
	}
#endif

	vch->frame_ts = ktime;
	if (!list_empty(&vch->buf_list)) {
		next = list_first_entry(&vch->buf_list, struct tw6869_buf, list);
    list_del(&next->list);
  
		if (pb) {
			done = vch->b_buf;
			vch->b_buf = next;
      addr =  R32_DMA_B_ADDR(id);
		} else {
			done = vch->p_buf;
			vch->p_buf = next;
      addr =  R32_DMA_P_ADDR(id);
		  done->vb.v4l2_buf.field = V4L2_FIELD_INTERLACED;
		}
	}

	if (done && next) {
		v4l2_get_timestamp(&done->vb.v4l2_buf.timestamp);
		done->vb.v4l2_buf.sequence = vch->sequence++;
		tw_write(dev, addr, next->dma);
		/* done->vb.v4l2_buf.field = (vch->std & V4L2_STD_625_50) ? V4L2_FIELD_INTERLACED_BT : V4L2_FIELD_INTERLACED_TB; */
		spin_unlock_irqrestore(&vch->lock, flags);
		vb2_buffer_done(&done->vb, VB2_BUF_STATE_DONE);
		TWINFO("vin/%u frame %u pb:%u ts:%ld.%06ld\n", id+1, vch->sequence, pb, done->vb.v4l2_buf.timestamp.tv_sec, done->vb.v4l2_buf.timestamp.tv_usec);
	} else {
		++vch->dcount;
		spin_unlock_irqrestore(&vch->lock, flags);
		TWINFO("vin/%u frame %u pb:%d last frame for %llu mysec (%llu) dropped (%u) - no next\n", id+1, vch->sequence, pb, ktime_us_delta(ktime, vch->frame_ts), ktime_to_ms(ktime), vch->dcount);
	}
  return 0;
}

#ifdef IMPL_AUDIO
static unsigned int tw6869_airq(struct tw6869_dev *dev,
				unsigned int id,
				unsigned int pb)
{
	unsigned long flags;
	struct tw6869_ach *ach = &dev->ach[ID2CH(id)];
	struct tw6869_buf *done = NULL;
	struct tw6869_buf *next = NULL;

	spin_lock_irqsave(&ach->lock, flags);
	if (!ach->ss || !ach->p_buf || !ach->b_buf) {
		spin_unlock_irqrestore(&ach->lock, flags);
		return TW_DMA_OFF;
	}

	if (ach->pb != pb) {
		ach->pb = 0;
		spin_unlock_irqrestore(&ach->lock, flags);
		dev_err(&dev->pdev->dev, "DMA channel %u bad bp - reset\n", id);
		return TW_DMA_RST;
	}

	if (!list_empty(&ach->buf_list)) {
		next = list_first_entry(&ach->buf_list, struct tw6869_buf, list);
		list_move_tail(&next->list, &ach->buf_list);
		if (pb) {
			done = ach->p_buf;
			ach->b_buf = next;
		} else {
			done = ach->b_buf;
			ach->p_buf = next;
		}
	}
	ach->pb = !pb;

	if (done && next) {
		tw_write(dev, pb ? R32_DMA_B_ADDR(id) : R32_DMA_P_ADDR(id), next->dma);
		ach->ptr = done->dma - ach->buf[0].dma;
		spin_unlock_irqrestore(&ach->lock, flags);
		snd_pcm_period_elapsed(ach->ss);
	} else {
		spin_unlock_irqrestore(&ach->lock, flags);
		return TW_DMA_OFF;
	}
	return 0;
}
#endif /* IMPL_AUDIO */

static irqreturn_t tw6869_irq(int irq, void *dev_id)
{
	struct tw6869_dev *dev = dev_id;
	unsigned long flags;
	unsigned int int_sts, fifo_sts, pb_sts, pars_sts, dma_cmd, id;
	ktime_t now = ktime_get();
	unsigned diff;
	unsigned int errBits;  /* erroneous DMA channels */
  unsigned active;

	spin_lock_irqsave(&dev->rlock, flags);
	int_sts = tw_read(dev, R32_INT_STATUS);
	fifo_sts = tw_read(dev, R32_FIFO_STATUS);
	pb_sts = tw_read(dev, R32_PB_STATUS);
	dev->dma_enable = tw_read(dev, R32_DMA_CHANNEL_ENABLE);
	pars_sts = tw_read(dev, R32_VIDEO_PARSER_STATUS);
	dma_cmd = tw_read(dev, R32_DMA_CMD);
	spin_unlock_irqrestore(&dev->rlock, flags);

	/* err_exist, such as cpl error, tlp error, time-out */
	errBits = ((int_sts >> 24) & 0xFF);
	errBits |= (((pars_sts >> 8) | pars_sts) & 0xFF);
	errBits |= (((fifo_sts >> 24) | (fifo_sts >> 16)) & 0xFF);
	errBits &= dev->dma_enable & ~dev->dma_disable;

  /* RSR 20180305 I observed, PB changes without set INT_STS bit for a long time. So I device to evaluate PB changes myself */
  active = (int_sts /*| (pb_sts ^ dev->pb_sts)*/) & dev->dma_enable & ~dev->dma_disable;
  dev->pb_sts = pb_sts;


	TWINFO("int_status:%08X fifo_status:%08X parser_status:%08X pb:%08X enabled:%02X disabled:%02X cmd:%08X errBits:%02X errMask:%02X\n",
			int_sts, fifo_sts, pars_sts, pb_sts, dev->dma_enable, dev->dma_disable, dma_cmd, errBits, dev->dma_error_mask);
	if (dev->dma_enable & (active|errBits) & TW_VID)
	{
		for (id = 0; id < TW_CH_MAX; id++) {
			if (errBits & BIT(id))
			{
				struct tw6869_vch *vch = &dev->vch[ID2CH(id)];
				if (0 == (vch->lcount & 0xFF))
				{
					TWWARN("vin/%u dma problems:%u drops %u seq:%u\n", id+1, vch->lcount, vch->dcount, vch->sequence);
				}
				vch->lcount++;
				if (dev->dma_error_mask & BIT(id)) {
					TWNOTICE("vin/%u ign dma error (%u)\n", id+1, vch->lcount);
					dev->dma_error_mask &= ~BIT(id);
					errBits &= ~BIT(id);
				} else {
					TWNOTICE("vin/%u dma error (%u)\n", id+1, vch->lcount);
					dev->dma_error_ts[id] = now;  /* set ts of the last error */
					dev->dma_disable |= BIT(id);
				}
			} else if (int_sts & BIT(id)) {
        if (dev->dma_error_mask & BIT(id)) 
        { 
          if (0 == tw6869_virq(dev, id, (pb_sts & BIT(id)))) {
            dev->dma_ok |= BIT(id);
          } else { 
            errBits |= BIT(id);
          }
        } else {
          dev->dma_error_mask |= BIT(id); 
        }
			} else if (dev->dma_ok & active & BIT(id)) {
	      unsigned video_status;
	      spin_lock_irqsave(&dev->rlock, flags);
	      video_status = tw_read(dev, R8_VIDEO_STATUS(id));
	      spin_unlock_irqrestore(&dev->rlock, flags);
	      if ( (0x80 & video_status) || ((video_status & 0x48) != 0x48) )
	      {
	          TWINFO("vch%u video status %02x (missing lock)\n", id, video_status);
        } else if (dev->dma_error_mask & BIT(id)) {
          if (0 == tw6869_virq(dev, id, (pb_sts & BIT(id)))) {
            dev->dma_ok |= BIT(id);
          } else {
            errBits |= BIT(id);
          }
        } else {
          dev->dma_error_mask |= BIT(id); 
        }
			}
		}
	}
#ifdef IMPL_AUDIO
	if (dev->dma_enable & int_sts & TW_AID) {
		for (; id < (2 * TW_CH_MAX); id++) {
			if (dev->dma_enable & int_sts & BIT(id)) {
				unsigned int cmd = tw6869_airq(dev, id, !!(pb_sts & BIT(id)));
				if (cmd) {
					spin_lock_irqsave(&dev->rlock, flags);
					tw6869_id_dma_cmd(dev, id, cmd);
					spin_unlock_irqrestore(&dev->rlock, flags);
				}
			}
		}
	}
#endif /* IMPL_AUDIO */

	if (errBits) {
#ifdef IRQ_DMA_STOP
		spin_lock_irqsave(&dev->rlock, flags);
		dev->dma_enable &= ~errBits;
		// stop  all error channels
		tw_write(dev, R32_DMA_CHANNEL_ENABLE, dev->dma_enable);
		dev->dma_enable = tw_read(dev, R32_DMA_CHANNEL_ENABLE);
		tw_write(dev, R32_DMA_CMD, BIT(31) | dev->dma_enable);
		tw_read(dev, R32_DMA_CMD);
		spin_unlock_irqrestore(&dev->rlock, flags);
		TWNOTICE("dma disable to %02X errs: %02X\n", dev->dma_enable, errBits);
#endif /* IRQ_DMA_STOP */
		mod_timer(&dev->dma_resync, jiffies + msecs_to_jiffies(5));
	}

	diff = ktime_us_delta(ktime_get(), now);
	TWINFO("ISR:%umysec\n", diff);
	if (diff > 1000)
	{
		TWWARN("tw6869 ISR %umysec\n", diff);
	}
	return IRQ_HANDLED;
}

/**********************************************************************/

static inline struct tw6869_buf *to_tw6869_buf(struct vb2_buffer *vb2)
{
	return container_of(vb2, struct tw6869_buf, vb);
}

static int to_tw6869_pixformat(unsigned int pixelformat)
{
	int ret;

	switch (pixelformat) {
	case V4L2_PIX_FMT_YUYV:
		ret = TW_FMT_YUYV;
		break;
	case V4L2_PIX_FMT_UYVY:
		ret = TW_FMT_UYVY;
		break;
	case V4L2_PIX_FMT_RGB565:
		ret = TW_FMT_RGB565;
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static unsigned int tw6869_fields_map(struct tw6869_vch *vch)
{
	unsigned int map[15] = {
		0x00000000, 0x00000001, 0x00004001, 0x00104001, 0x00404041,
		0x01041041, 0x01104411, 0x01111111, 0x04444445, 0x04511445,
		0x05145145, 0x05151515, 0x05515455, 0x05551555, 0x05555555
	};

	unsigned int std_625_50[26] = {
		0, 1, 1, 2,  3,  3,  4,  4,  5,  5,  6,  7,  7,
		   8, 8, 9, 10, 10, 11, 11, 12, 13, 13, 14, 14, 0
	};

	unsigned int std_525_60[31] = {
		0, 1, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,  6, 7, 7,
		   8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 0, 0
	};

	unsigned int i;
	if (vch->std & V4L2_STD_625_50) {
		vch->fps = (vch->fps > 25) ? 25 : vch->fps;
		i = std_625_50[vch->fps];
	} else {
		vch->fps = (vch->fps > 30) ? 30 : vch->fps;
		i = std_525_60[vch->fps];
	}

	return map[i];
}

static void tw6869_vch_frame_period(struct tw6869_vch *vch,
		struct v4l2_fract *frameperiod)
{
	if (vch->std & V4L2_STD_625_50) {
		frameperiod->numerator = 1;
		frameperiod->denominator = vch->fps;
	} else {
		frameperiod->numerator = 1001;
		frameperiod->denominator = vch->fps * 1000;
	}
}

static void tw6869_fill_pix_format(struct tw6869_vch *vch,
				struct v4l2_pix_format *pix)
{
	pix->width = 720;
	pix->height = (vch->std & V4L2_STD_625_50) ? 576 : 480;
	/* pix->field = (vch->std & V4L2_STD_625_50) ? V4L2_FIELD_INTERLACED_BT : V4L2_FIELD_INTERLACED_TB; */
	pix->field = V4L2_FIELD_INTERLACED;
	pix->pixelformat = V4L2_PIX_FMT_UYVY;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
	pix->bytesperline = pix->width * 2;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->priv = 0;
}

static void tw6869_vch_set_dma(struct tw6869_vch *vch)
{
	struct tw6869_dev *dev = vch->dev;
	struct v4l2_pix_format *pix = &vch->format;
	unsigned int id = vch->id;
	unsigned int cfg;

	if (!vch->fps)
	{
		vch->fps = (vch->std & V4L2_STD_625_50) ? 25 : 30;
		tw_write(dev, R32_VIDEO_FIELD_CTRL(id), 0);
	} else {
		unsigned int map = tw6869_fields_map(vch) << 1;
		map |= map << 1;
		if (map > 0) { map |= BIT(31); }
		tw_write(vch->dev, R32_VIDEO_FIELD_CTRL(vch->id), map);
	}

	if (vch->std & V4L2_STD_625_50)
		tw_set(dev, R32_VIDEO_CONTROL1, BIT(ID2CH(id)) << 13);
	else
		tw_clear(dev, R32_VIDEO_CONTROL1, BIT(ID2CH(id)) << 13);

	cfg = BIT(27) | ((to_tw6869_pixformat(pix->pixelformat) & 0x7) << 20);
	tw_write(dev, R32_DMA_CHANNEL_CONFIG(id), cfg);

	cfg = (((pix->height >> 1) & 0x3FF) << 22) |
		((pix->bytesperline & 0x7FF) << 11) |
		(pix->bytesperline & 0x7FF);
	tw_write(dev, R32_DMA_WHP(id), cfg);
}

/*
 * Videobuf2 Operations
 */
static int queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], void *alloc_ctxs[])
{
	struct tw6869_vch *vch = vb2_get_drv_priv(vq);
	struct tw6869_dev *dev = vch->dev;

	if (vq->num_buffers + *nbuffers < TW_FRAME_MAX)
		*nbuffers = TW_FRAME_MAX - vq->num_buffers;

	if (fmt && fmt->fmt.pix.sizeimage < vch->format.sizeimage)
		return -EINVAL;
	*nplanes = 1;
	sizes[0] = fmt ? fmt->fmt.pix.sizeimage : vch->format.sizeimage;
	alloc_ctxs[0] = dev->alloc_ctx;
	return 0;
}

static int buffer_init(struct vb2_buffer *vb)
{
	struct tw6869_buf *buf = to_tw6869_buf(vb);

	buf->dma = vb2_dma_contig_plane_dma_addr(vb, 0);
	INIT_LIST_HEAD(&buf->list);
	{	/* this hack to make gst-fsl-plugins happy */
		unsigned int *cpu = vb2_plane_vaddr(vb, 0);
		*cpu = buf->dma;
	}
	return 0;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct tw6869_vch *vch = vb2_get_drv_priv(vb->vb2_queue);
	struct tw6869_dev *dev = vch->dev;
	struct tw6869_buf *buf = to_tw6869_buf(vb);
	unsigned long size = vch->format.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		v4l2_err(&dev->v4l2_dev, "buffer too small (%lu < %lu)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}
	vb2_set_plane_payload(&buf->vb, 0, size);
	return 0;
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct tw6869_vch *vch = vb2_get_drv_priv(vb->vb2_queue);
	struct tw6869_buf *buf = to_tw6869_buf(vb);
	unsigned long flags;

	spin_lock_irqsave(&vch->lock, flags);
	list_add_tail(&buf->list, &vch->buf_list);
	spin_unlock_irqrestore(&vch->lock, flags);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct tw6869_vch *vch = vb2_get_drv_priv(vq);
	struct tw6869_dev *dev = vch->dev;
	struct tw6869_buf *p_buf, *b_buf;
	unsigned long flags;

	spin_lock_irqsave(&vch->lock, flags);
	if (list_empty(&vch->buf_list) ||
			(vch->buf_list.prev == vch->buf_list.next)) {
		spin_unlock_irqrestore(&vch->lock, flags);
		return -ENOBUFS;
	}

	p_buf = list_first_entry(&vch->buf_list, struct tw6869_buf, list);
	list_del(&p_buf->list);
	vch->p_buf = p_buf;
	tw_write(dev, R32_DMA_P_ADDR(vch->id), p_buf->dma);

	b_buf = list_first_entry(&vch->buf_list, struct tw6869_buf, list);
	list_del(&b_buf->list);
	vch->b_buf = b_buf;
	tw_write(dev, R32_DMA_B_ADDR(vch->id), b_buf->dma);

	vch->sequence = 0;
	vch->dcount = 0;
	vch->lcount = 0;
	spin_unlock_irqrestore(&vch->lock, flags);

	TWINFO("vin/%u activated\n", vch->id+1);
	spin_lock_irqsave(&dev->rlock, flags);
	tw6869_id_dma_cmd(dev, vch->id, TW_DMA_ON);
	spin_unlock_irqrestore(&dev->rlock, flags);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
	#define STOP_STREAMING_RETURN_TYPE void
	#define STOP_STREAMING_RETURN_VALUE
#else
	#define STOP_STREAMING_RETURN_TYPE int
	#define STOP_STREAMING_RETURN_VALUE 0
#endif

static STOP_STREAMING_RETURN_TYPE stop_streaming(struct vb2_queue *vq)
{
	struct tw6869_vch *vch = vb2_get_drv_priv(vq);
	struct tw6869_dev *dev = vch->dev;
	struct tw6869_buf *buf, *node;
	unsigned long flags;

	TWINFO("vin/%u stopped\n", vch->id+1);
	spin_lock_irqsave(&dev->rlock, flags);
	tw6869_id_dma_cmd(dev, vch->id, TW_DMA_OFF);
	spin_unlock_irqrestore(&dev->rlock, flags);

	spin_lock_irqsave(&vch->lock, flags);
	if (vch->p_buf) {
		buf = vch->p_buf;
		vch->p_buf = NULL;
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
	}

	if (vch->b_buf) {
		buf = vch->b_buf;
		vch->b_buf = NULL;
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
	}

	list_for_each_entry_safe(buf, node, &vch->buf_list, list) {
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&vch->lock, flags);

	return STOP_STREAMING_RETURN_VALUE;
}

static struct vb2_ops tw6869_qops = {
	.queue_setup		= queue_setup,
	.buf_init		= buffer_init,
	.buf_prepare		= buffer_prepare,
	.buf_queue		= buffer_queue,
	.start_streaming	= start_streaming,
	.stop_streaming		= stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static int tw6869_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	struct tw6869_vch *vch = video_drvdata(file);
	struct tw6869_dev *dev = vch->dev;

	strlcpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "tw6869 vch%u", ID2CH(vch->id));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		pci_name(dev->pdev));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
		V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int tw6869_try_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct tw6869_vch *vch = video_drvdata(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;
	int ret;

	ret = to_tw6869_pixformat(pix->pixelformat);
	if (ret < 0)
		return ret;

	tw6869_fill_pix_format(vch, pix);
	return 0;
}

static int tw6869_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct tw6869_vch *vch = video_drvdata(file);
	int ret;

	ret = tw6869_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	if (vb2_is_busy(&vch->queue))
		return -EBUSY;

	vch->format = f->fmt.pix;
	tw6869_vch_set_dma(vch);
	return 0;
}

static int tw6869_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct tw6869_vch *vch = video_drvdata(file);

	f->fmt.pix = vch->format;
	return 0;
}

static int tw6869_enum_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{

	if (f->index > 2)
		return -EINVAL;

	switch (f->index) {
	case 1:
		strlcpy(f->description, "4:2:2, packed, YUYV", sizeof(f->description) );
		f->pixelformat = V4L2_PIX_FMT_YUYV;
		break;
	case 2:
		strlcpy(f->description, "16 bpp RGB, le", sizeof(f->description) );
		f->pixelformat = V4L2_PIX_FMT_RGB565;
		break;
	default:
		strlcpy(f->description, "4:2:2, packed, UYVY", sizeof(f->description) );
		f->pixelformat = V4L2_PIX_FMT_UYVY;
	}
	f->flags = 0;
	return 0;
}

static const struct v4l2_frmsize_discrete ntsc_sizes[] = {
	{ 720, 480 },
	{ 720, 240 },
};

static const struct v4l2_frmsize_discrete pal_sizes[] = {
	{ 720, 576 },
	{ 720, 288 },
};

#define NUM_SIZE_ENUMS (ARRAY_SIZE(pal_sizes))

static int tw6869_enum_framesizes(struct file *file, void *priv,
                             struct v4l2_frmsizeenum *fe)
{
	const struct tw6869_vch *vch = video_drvdata(file);
	//const struct tw6869_dev *dev = vch->dev;
	const int is_ntsc = vch->std & V4L2_STD_525_60;

	int ret = to_tw6869_pixformat(fe->pixel_format);
	if(ret < 0)
		return -EINVAL;
	
	if (fe->index >= NUM_SIZE_ENUMS)
		return -EINVAL;

	fe->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fe->discrete = is_ntsc ? ntsc_sizes[fe->index] : pal_sizes[fe->index];

	return 0;
}

static int tw6869_enum_frameintervals(struct file *file, void *priv,
		struct v4l2_frmivalenum *fival)
{
	struct tw6869_vch *vch = video_drvdata(file);

	if (fival->index != 0 ||
		to_tw6869_pixformat(fival->pixel_format) < 0)
		return -EINVAL;

	tw6869_vch_frame_period(vch, &fival->discrete);
	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	return 0;
}

static int tw6869_querystd(struct file *file, void *priv, v4l2_std_id *std)
{
	struct tw6869_vch *vch = video_drvdata(file);
	struct tw6869_dev *dev = vch->dev;
	unsigned int std_now;
	char *std_str;
	unsigned char video_status;

	std_now = tw_read(dev, R8_STANDARD_SEL(vch->id));
	video_status = tw_read(dev, R8_VIDEO_STATUS(vch->id));

	if ( (0x80 & video_status) || ((video_status & 0x48) != 0x48) )
	{
	  TWINFO("vch%u video status 0x%02x std:%02x (missing lock)\n", ID2CH(vch->id), video_status, std_now);
		std_str = "video not present";
		*std = V4L2_STD_UNKNOWN;
		return 0;
	} else {
	  TWINFO("vch%u video status 0x%02x std:%02x\n", ID2CH(vch->id), video_status, std_now);
  }

	std_now &= (0x07 << 4);
	std_now >>= 4;


	switch (std_now) {
	case TW_STD_NTSC_M:
		std_str = "NTSC (M)";	// U.S., many others
		*std = V4L2_STD_NTSC_M;
		break;
	case TW_STD_PAL:
		std_str = "PAL (B,D,G,H,I)";	// Western Europe, many others
		*std = (V4L2_STD_PAL_B | V4L2_STD_PAL_D | V4L2_STD_PAL_G | V4L2_STD_PAL_H | V4L2_STD_PAL_I);
		break;
	case TW_STD_SECAM:
		std_str = "SECAM";	// France, Eastern Europe, Middle East, Russia
		*std = V4L2_STD_SECAM;
		break;
	case TW_STD_NTSC_443:
		std_str = "NTSC 4.43";	// Transcoding
		*std = V4L2_STD_NTSC_443;
		break;
	case TW_STD_PAL_M:
		std_str = "PAL (M)";	// Brazil
		*std = V4L2_STD_PAL_M;
		break;
	case TW_STD_PAL_CN:
		std_str = "PAL (CN)";	// Argentina
		*std = V4L2_STD_PAL_Nc;
		break;
	case TW_STD_PAL_60:
		std_str = "PAL 60";	// China
		*std = V4L2_STD_PAL_60;
		break;
	case TW_STD_NOT_VALID:
		std_str = "Not valid";
		*std = V4L2_STD_UNKNOWN;
	}
	return 0;
}

static int tw6869_s_std(struct file *file, void *priv, v4l2_std_id std)
{
	struct tw6869_vch *vch = video_drvdata(file);
	v4l2_std_id new_std = (std & V4L2_STD_625_50) ?
				V4L2_STD_625_50 : V4L2_STD_525_60;

	if (new_std == vch->std)
		return 0;

	if (vb2_is_busy(&vch->queue))
		return -EBUSY;

	vch->std = new_std;
	tw6869_fill_pix_format(vch, &vch->format);
	tw6869_vch_set_dma(vch);
	return 0;
}

static int tw6869_g_std(struct file *file, void *priv, v4l2_std_id *std)
{
	struct tw6869_vch *vch = video_drvdata(file);
	v4l2_std_id new_std = 0;

	tw6869_querystd(file, priv, &new_std);
	if (new_std)
		tw6869_s_std(file, priv, new_std);

	*std = vch->std;
	return 0;
}

/* SK-TW6869: only input0 is available */
static int tw6869_enum_input(struct file *file, void *priv,
				struct v4l2_input *i)
{

	if (i->index < TW_VIN_MAX) {
		i->type = V4L2_INPUT_TYPE_CAMERA;
		i->std = V4L2_STD_ALL;
		sprintf(i->name, "Camera %u", i->index);
		return 0;
	}
	return -EINVAL;
}

static int tw6869_s_input(struct file *file, void *priv, unsigned int input)
{
	struct tw6869_vch *vch = video_drvdata(file);
	
	if (input < TW_VIN_MAX) {
		vch->input = input;
		return 0;
	}
	v4l2_err(&vch->dev->v4l2_dev, "tw6869_s_input failed input=%u\n", input );
	return -EINVAL;
}

static int tw6869_g_input(struct file *file, void *priv, unsigned int *input)
{
	struct tw6869_vch *vch = video_drvdata(file);

	*input = vch->input;
	return 0;
}

static int tw6869_g_parm(struct file *file, void *priv,
				struct v4l2_streamparm *sp)
{
  struct tw6869_vch *vch = video_drvdata(file);
  struct v4l2_captureparm *cp = &sp->parm.capture;

  if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
    return -EINVAL;

  memset(cp, 0, sizeof(*cp));
  cp->capability = V4L2_CAP_TIMEPERFRAME;
  tw6869_vch_frame_period(vch, &cp->timeperframe);
  return 0;
}

static int tw6869_s_parm(struct file *file, void *priv,
				struct v4l2_streamparm *sp)
{
  struct tw6869_vch *vch = video_drvdata(file);
  struct v4l2_captureparm *cp = &sp->parm.capture;
  unsigned int denominator = cp->timeperframe.denominator;
  unsigned int numerator = cp->timeperframe.numerator;
  unsigned int fps;

  if( sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE )
    return -EINVAL;

  if (vb2_is_streaming(&vch->queue))
    return -EBUSY;

  fps = (!numerator || !denominator) ? 0 : denominator / numerator;
  if (vch->std & V4L2_STD_625_50)
    fps = (!fps || fps > 25) ? 25 : fps;
  else
    fps = (!fps || fps > 30) ? 30 : fps;

  if (vch->fps != fps) {
    unsigned int map;
    vch->fps = fps;
    map = tw6869_fields_map(vch) << 1;
    map |= map << 1;
    if (map > 0)
      map |= BIT(31);
    tw_write(vch->dev, R32_VIDEO_FIELD_CTRL(vch->id), map);
    /*RSR v4l2_info(&vch->dev->v4l2_dev,
      "vch%u fps %u\n", ID2CH(vch->id), vch->fps); */
  }
  return tw6869_g_parm(file, priv, sp);
}

/* The control handler. */
static int tw6869_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct tw6869_vch *vch =
		container_of(ctrl->handler, struct tw6869_vch, hdl);
	struct tw6869_dev *dev = vch->dev;
	unsigned int id = vch->id;
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		tw_write(dev, R8_BRIGHT_CTRL(id), ctrl->val);
		break;
	case V4L2_CID_CONTRAST:
		tw_write(dev, R8_CONTRAST_CTRL(id), ctrl->val);
		break;
	case V4L2_CID_SATURATION:
		tw_write(dev, R8_SAT_U_CTRL(id), ctrl->val);
		tw_write(dev, R8_SAT_V_CTRL(id), ctrl->val);
		break;
	case V4L2_CID_HUE:
		tw_write(dev, R8_HUE_CTRL(id), ctrl->val);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

/*
 * File operations for the device
 */
static const struct v4l2_ctrl_ops tw6869_ctrl_ops = {
	.s_ctrl = tw6869_s_ctrl,
};

static int tw6869_ioctl_querybuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	int ret;
	struct video_device *vdev = video_devdata(file);

	/* No need to call vb2_queue_is_busy(), anyone can query buffers. */
	ret = vb2_querybuf(vdev->queue, p);
	
	if (!ret) {
		/* return physical address */
		struct vb2_buffer *vb = vdev->queue->bufs[p->index];
		if (p->flags & V4L2_BUF_FLAG_MAPPED)
			p->m.offset = vb2_dma_contig_plane_dma_addr(vb, 0);
	}
	return ret;
};

static const struct v4l2_ioctl_ops tw6869_ioctl_ops = {
	.vidioc_querycap = tw6869_querycap,
	.vidioc_try_fmt_vid_cap = tw6869_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = tw6869_s_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = tw6869_g_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap = tw6869_enum_fmt_vid_cap,

	.vidioc_g_std = tw6869_g_std,
	.vidioc_s_std = tw6869_s_std,
	.vidioc_querystd = tw6869_querystd,

	.vidioc_enum_input = tw6869_enum_input,
	.vidioc_g_input = tw6869_g_input,
	.vidioc_s_input = tw6869_s_input,

	.vidioc_enum_framesizes = tw6869_enum_framesizes,
	.vidioc_enum_frameintervals = tw6869_enum_frameintervals,

	.vidioc_g_parm = tw6869_g_parm,
	.vidioc_s_parm = tw6869_s_parm,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_querybuf = tw6869_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations tw6869_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.read = vb2_fop_read,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int tw6869_vch_register(struct tw6869_vch *vch)
{
	struct tw6869_dev *dev = vch->dev;
	struct v4l2_ctrl_handler *hdl = &vch->hdl;
	struct vb2_queue *q = &vch->queue;
	struct video_device *vdev = &vch->vdev;
	int ret = 0;

	/* Add the controls */
	v4l2_ctrl_handler_init(hdl, 4);
	v4l2_ctrl_new_std(hdl, &tw6869_ctrl_ops,
		  V4L2_CID_BRIGHTNESS, -128, 127, 1, 0);
	v4l2_ctrl_new_std(hdl, &tw6869_ctrl_ops,
		  V4L2_CID_CONTRAST, 0, 255, 1, 100);
	v4l2_ctrl_new_std(hdl, &tw6869_ctrl_ops,
		  V4L2_CID_SATURATION, 0, 255, 1, 128);
	v4l2_ctrl_new_std(hdl, &tw6869_ctrl_ops,
		  V4L2_CID_HUE, -128, 127, 1, 0);
	if (hdl->error) {
		printk("handler FAILED!\n");
		ret = hdl->error;
		return ret;
	}

	/* Fill in the initial format-related settings */
	vch->std = V4L2_STD_625_50;
	vch->format.pixelformat = V4L2_PIX_FMT_UYVY;
	tw6869_fill_pix_format(vch, &vch->format);
	tw6869_vch_set_dma(vch);

	mutex_init(&vch->mlock);

	/* Initialize the vb2 queue */
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	q->drv_priv = vch;
	q->buf_struct_size = sizeof(struct tw6869_buf);
	q->ops = &tw6869_qops;
	q->mem_ops = &vb2_dma_contig_memops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 15)
	q->timestamp_flags |= V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
	q->timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
#endif
	q->lock = &vch->mlock;
	q->gfp_flags = __GFP_DMA32;
	ret = vb2_queue_init(q);
	if (ret) {
		printk("vb2_queue_init FAILED!\n");
		goto free_hdl;
	}

	spin_lock_init(&vch->lock);
	INIT_LIST_HEAD(&vch->buf_list);

	/* Initialize the video_device structure */
	strlcpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->fops = &tw6869_fops,
	vdev->ioctl_ops = &tw6869_ioctl_ops,
	vdev->lock = &vch->mlock;
	vdev->queue = q;
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->ctrl_handler = hdl;
	vdev->tvnorms = V4L2_STD_ALL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
	vdev->debug = 0;
#endif
	video_set_drvdata(vdev, vch);
	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (!ret) {
		return 0;
	}
	printk("video_register_device FAILED!\n");

free_hdl:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static void tw6869_video_unregister(struct tw6869_dev *dev)
{
	unsigned int i;

	/* Reset and disable all DMA channels */
	tw_write(dev, R32_DMA_CMD, 0);
	tw_write(dev, R32_DMA_CHANNEL_ENABLE, 0);

	if (!dev->alloc_ctx)
		return;

	if (dev->ch_max > TW_CH_MAX)
		dev->ch_max = TW_CH_MAX;

	for (i = 0; i < dev->ch_max; i++) {
		struct tw6869_vch *vch = &dev->vch[ID2CH(i)];
		video_unregister_device(&vch->vdev);
		v4l2_ctrl_handler_free(&vch->hdl);
	}

	v4l2_device_unregister(&dev->v4l2_dev);
	vb2_dma_contig_cleanup_ctx(dev->alloc_ctx);
	dev->alloc_ctx = NULL;
}

static int tw6869_video_register(struct tw6869_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	unsigned int i;
	int ret;

	/* Initialize the top-level structure */
	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	dev->alloc_ctx = vb2_dma_contig_init_ctx(&pdev->dev);
	if (IS_ERR(dev->alloc_ctx)) {
		ret = PTR_ERR(dev->alloc_ctx);
		v4l2_err(&dev->v4l2_dev, "can't allocate buffer context\n");
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->alloc_ctx = NULL;
		return ret;
	}

	for (i = 0; i < TW_CH_MAX; i++) {
		struct tw6869_vch *vch = &dev->vch[ID2CH(i)];
		vch->dev = dev;
		vch->id = i;
		ret = tw6869_vch_register(vch);
		if (ret) {
			dev->ch_max = i;
			tw6869_video_unregister(dev);
			return ret;
		}
	}
	return 0;
}

/**********************************************************************/

#ifdef IMPL_AUDIO
static int tw6869_pcm_hw_params(struct snd_pcm_substream *ss,
				struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(hw_params));
}

static int tw6869_pcm_hw_free(struct snd_pcm_substream *ss)
{
	return snd_pcm_lib_free_pages(ss);
}

/* TODO: SNDRV_PCM_RATE_8000_48000 */
static const struct snd_pcm_hardware tw6869_capture_hw = {
	.info			= (SNDRV_PCM_INFO_MMAP |
				   SNDRV_PCM_INFO_INTERLEAVED |
				   SNDRV_PCM_INFO_BLOCK_TRANSFER |
				   SNDRV_PCM_INFO_MMAP_VALID),
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_48000,
	.rate_min		= 48000,
	.rate_max		= 48000,
	.channels_min		= 1,
	.channels_max		= 1,
	.buffer_bytes_max	= TW_APAGE_MAX * TW_PAGE_SIZE,
	.period_bytes_min	= TW_PAGE_SIZE,
	.period_bytes_max	= TW_PAGE_SIZE,
	.periods_min		= 2,
	.periods_max		= TW_APAGE_MAX,
};

static int tw6869_pcm_open(struct snd_pcm_substream *ss)
{
	struct tw6869_dev *dev = snd_pcm_substream_chip(ss);
	struct tw6869_ach *ach = &dev->ach[ID2CH(ss->number)];
	struct snd_pcm_runtime *rt = ss->runtime;
	int ret;

	ach->ss = ss;
	rt->hw = tw6869_capture_hw;

	ret = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	return 0;
}

static int tw6869_pcm_close(struct snd_pcm_substream *ss)
{
	struct tw6869_dev *dev = snd_pcm_substream_chip(ss);
	struct tw6869_ach *ach = &dev->ach[ID2CH(ss->number)];

	ach->ss = NULL;
	return 0;
}

static int tw6869_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct tw6869_dev *dev = snd_pcm_substream_chip(ss);
	struct tw6869_ach *ach = &dev->ach[ID2CH(ss->number)];
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned int period = snd_pcm_lib_period_bytes(ss);
	struct tw6869_buf *p_buf, *b_buf;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&dev->rlock, flags);
	tw6869_id_dma_cmd(dev, ach->id, TW_DMA_OFF);
	spin_unlock_irqrestore(&dev->rlock, flags);

	if ((period != TW_PAGE_SIZE) ||
		(rt->periods < 2) ||
		(rt->periods > TW_APAGE_MAX)) {
		return -EINVAL;
	}

	spin_lock_irqsave(&ach->lock, flags);
	INIT_LIST_HEAD(&ach->buf_list);

	for (i = 0; i < rt->periods; i++) {
		ach->buf[i].dma = rt->dma_addr + period * i;
		INIT_LIST_HEAD(&ach->buf[i].list);
		list_add_tail(&ach->buf[i].list, &ach->buf_list);
	}

	p_buf =	list_first_entry(&ach->buf_list, struct tw6869_buf, list);
	list_move_tail(&p_buf->list, &ach->buf_list);
	ach->p_buf = p_buf;
	tw_write(dev, R32_DMA_P_ADDR(ach->id), p_buf->dma);

	b_buf =	list_first_entry(&ach->buf_list, struct tw6869_buf, list);
	list_move_tail(&b_buf->list, &ach->buf_list);
	ach->b_buf = b_buf;
	tw_write(dev, R32_DMA_B_ADDR(ach->id), b_buf->dma);

	ach->ptr = 0;
	ach->pb = 0;
	spin_unlock_irqrestore(&ach->lock, flags);

	return 0;
}

static int tw6869_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct tw6869_dev *dev = snd_pcm_substream_chip(ss);
	struct tw6869_ach *ach = &dev->ach[ID2CH(ss->number)];
	unsigned long flags;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (ach->p_buf && ach->b_buf) {
			spin_lock_irqsave(&dev->rlock, flags);
			tw6869_id_dma_cmd(dev, ach->id, TW_DMA_ON);
			spin_unlock_irqrestore(&dev->rlock, flags);
		} else {
			ret = -EIO;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&dev->rlock, flags);
		tw6869_id_dma_cmd(dev, ach->id, TW_DMA_OFF);
		spin_unlock_irqrestore(&dev->rlock, flags);

		spin_lock_irqsave(&ach->lock, flags);
		ach->p_buf = NULL;
		ach->b_buf = NULL;
		spin_unlock_irqrestore(&ach->lock, flags);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static snd_pcm_uframes_t tw6869_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct tw6869_dev *dev = snd_pcm_substream_chip(ss);
	struct tw6869_ach *ach = &dev->ach[ID2CH(ss->number)];

	return bytes_to_frames(ss->runtime, ach->ptr);
}

static struct snd_pcm_ops tw6869_pcm_ops = {
	.open = tw6869_pcm_open,
	.close = tw6869_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tw6869_pcm_hw_params,
	.hw_free = tw6869_pcm_hw_free,
	.prepare = tw6869_pcm_prepare,
	.trigger = tw6869_pcm_trigger,
	.pointer = tw6869_pcm_pointer,
};

static int tw6869_snd_pcm_init(struct tw6869_dev *dev)
{
	struct snd_card *card = dev->snd_card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *ss;
	unsigned int i;
	int ret;

	ret = snd_pcm_new(card, card->driver, 0, 0, TW_CH_MAX, &pcm);
	if (ret < 0)
		return ret;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tw6869_pcm_ops);
	snd_pcm_chip(pcm) = dev;
	pcm->info_flags = 0;
	strcpy(pcm->name, card->shortname);

	for (i = 0, ss = pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	     ss; ss = ss->next, i++)
		sprintf(ss->name, "vch%u audio", i);

	return snd_pcm_lib_preallocate_pages_for_all(pcm,
				SNDRV_DMA_TYPE_DEV,
				snd_dma_pci_data(dev->pdev),
				TW_APAGE_MAX * TW_PAGE_SIZE,
				TW_APAGE_MAX * TW_PAGE_SIZE);
}

static void tw6869_audio_unregister(struct tw6869_dev *dev)
{
	/* Reset and disable audio DMA */
	tw_clear(dev, R32_DMA_CMD, TW_AID);
	tw_clear(dev, R32_DMA_CHANNEL_ENABLE, TW_AID);

	if (!dev->snd_card)
		return;

	snd_card_free(dev->snd_card);
	dev->snd_card = NULL;
}

/* TODO: mixer controls */
static int tw6869_audio_register(struct tw6869_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	static struct snd_device_ops ops = { NULL };
	struct snd_card *card;
	unsigned int i;
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0)
	ret = snd_card_new(NULL, SNDRV_DEFAULT_IDX1, KBUILD_MODNAME,
				THIS_MODULE, 0, &card);
#else
	ret = snd_card_create(SNDRV_DEFAULT_IDX1, KBUILD_MODNAME,
				THIS_MODULE, 0, &card);
#endif
	if (ret < 0)
		return ret;

	dev->snd_card = card;

	for (i = 0; i < TW_CH_MAX; i++) {
		struct tw6869_ach *ach = &dev->ach[ID2CH(i)];
		spin_lock_init(&ach->lock);
		ach->dev = dev;
		ach->id = i + TW_CH_MAX;
	}

	strcpy(card->driver, KBUILD_MODNAME);
	strcpy(card->shortname, KBUILD_MODNAME);
	sprintf(card->longname, "%s on %s IRQ %d", card->shortname,
		pci_name(pdev), pdev->irq);

	ret = snd_device_new(card, SNDRV_DEV_LOWLEVEL, dev, &ops);
	if (ret < 0)
		goto snd_error;

	snd_card_set_dev(card, &pdev->dev);

	ret = tw6869_snd_pcm_init(dev);
	if (ret < 0)
		goto snd_error;

	ret = snd_card_register(card);
	if (!ret)
		return 0;

snd_error:
	snd_card_free(card);
	dev->snd_card = NULL;
	return ret;
}
#endif /* IMPL_AUDIO */

/**********************************************************************/


/* periodic DMA reset observer */
static void dma_resync(unsigned long data)
{
	struct tw6869_dev *dev = (struct tw6869_dev* )data;
	unsigned mask;
	unsigned long flags;
	ktime_t now = ktime_get();

#ifdef EARLY_DISABLE
	if (dev->dma_disable)
  {
				TWNOTICE("disable DMA %02X: %02X\n", dev->dma_enable, dev->dma_disable);
				spin_lock_irqsave(&dev->rlock, flags);
			  tw_write(dev, R32_DMA_CHANNEL_ENABLE, dev->dma_enable & ~dev->dma_disable);
			  dev->dma_enable = tw_read(dev, R32_DMA_CHANNEL_ENABLE);
        if (dev->dma_enable)
        {
          tw_write(dev, R32_DMA_CMD, BIT(31) | dev->dma_enable); 
        } else {
          tw_write(dev, R32_DMA_CMD, 0);
        }
			  tw_read(dev, R32_DMA_CMD);
        dev->dma_disable = 0;
				spin_unlock_irqrestore(&dev->rlock, flags);
  }
#endif /* EARLY_DISABLE */

	mask = (((dev->dma_enable & ~dev->dma_disable) ^ dev->videoCap_ID)) & dev->videoCap_ID;
	if (mask)
	{
		unsigned count;
		for (count = 0; count < TW_CH_MAX; count++)
		{
#ifdef LAST_ENABLE
			unsigned id = (count + dev->dma_last_enable + 1) % TW_CH_MAX;
#else
			unsigned id = count;
#endif /* LAST_ENABLE */

			if ((mask & BIT(id)) && (ktime_us_delta(now, dev->dma_error_ts[id]) > 30000)) {
				/* enable DMA channels */
				TWNOTICE("vin/%u re-enable %02X/%02X/%02X\n", id+1, dev->dma_enable, dev->dma_disable, dev->videoCap_ID);
				spin_lock_irqsave(&dev->rlock, flags);
#ifdef EARLY_DISABLE
				tw_read(dev, R32_DMA_P_ADDR(id));
				tw_read(dev, R32_DMA_B_ADDR(id));
				if (dev->vch[id].b_buf) { tw_write(dev, R32_DMA_B_ADDR(id), dev->vch[id].b_buf->dma); }
				if (dev->vch[id].p_buf) { tw_write(dev, R32_DMA_P_ADDR(id), dev->vch[id].p_buf->dma); }
#else
				tw6869_id_dma_cmd(dev, id, TW_DMA_RST);
				dev->dma_disable &= ~BIT(id);
#endif /* EARLY_DISABLE */

			  tw_write(dev, R32_DMA_CHANNEL_ENABLE, dev->dma_enable | BIT(id));
				dev->dma_enable = tw_read(dev, R32_DMA_CHANNEL_ENABLE);
			  tw_write(dev, R32_DMA_CMD, BIT(31) | dev->dma_enable);
				dev->dma_error_mask |= BIT(id);
				dev->dma_ok &= ~BIT(id);
				tw_read(dev, R32_DMA_CMD);

#ifdef LAST_ENABLE
				dev->dma_last_enable = id;
#endif /* LAST_ENABLE */
				spin_unlock_irqrestore(&dev->rlock, flags);
				break;
			}
		}
		mod_timer(&dev->dma_resync, jiffies + msecs_to_jiffies(20));
	} else {
#ifdef IRQ_DMA_STOP
		TWINFO("enable %02X/%02X timer down\n", dev->dma_enable, dev->videoCap_ID);
#else
		TWNOTICE("re-enable %02X/%02X/%02X stopped\n", dev->dma_enable, dev->dma_disable, dev->videoCap_ID);
		//mod_timer(&dev->dma_resync, jiffies + msecs_to_jiffies(20));
#endif /* IRQ_DMA_STOP */
	}
}



static int tw6869_reset(struct tw6869_dev *dev)
{
	unsigned id;
	u32 regDW;

	/* Disable all DMA channels */
	tw_write(dev, R32_DMA_CHANNEL_ENABLE, 0);
	mdelay(50);
	/* Reset all DMA channels */
	tw_write(dev, R32_DMA_CMD, 0);
	tw_read(dev, R32_DMA_CHANNEL_ENABLE);
	tw_read(dev, R32_DMA_CMD);

	tw_write(dev, EP_REG_ADDR, 0x730);
	regDW = tw_read(dev, EP_REG_ADDR);
	if (regDW != 0x730) {
		dev_err(&dev->pdev->dev, "expected 0x730, read 0x%x\n", regDW);
		return -ENODEV;
	}
	regDW = tw_read(dev, EP_REG_DATA);
	dev_info(&dev->pdev->dev, "PCI_CFG[Posted 0x730]=0x%x\n", regDW);

	//Trasnmit Non-Posted FC credit Status
	tw_write(dev, EP_REG_ADDR, 0x734);   //
	regDW = tw_read(dev, EP_REG_DATA );
	dev_info(&dev->pdev->dev, "PCI_CFG[Non-Posted 0x734]=0x%x\n", regDW );

	//CPL FC credit Status
	tw_write(dev, EP_REG_ADDR, 0x738);   //
	regDW = tw_read(dev, EP_REG_DATA );
	dev_info(&dev->pdev->dev, "PCI_CFG[CPL 0x738]=0x%x\n", regDW );

	tw_read(dev, R32_SYS_SOFT_RST);
	tw_write(dev, R32_SYS_SOFT_RST, 0x01);
	mdelay(50);
	tw_write(dev, R32_SYS_SOFT_RST, 0x0F);
	tw_read(dev, R32_SYS_SOFT_RST);

	/* 625 lines, full D1 */
	tw_write(dev, R32_VIDEO_CONTROL1, 0x001FE001);

	for(id = 0; id < TW_CH_MAX; id++)
	{
		//tw_write(dev, R8_STANDARD_REC(id), 0xFF);
		tw_write(dev, R8_STANDARD_REC(id), 0x81); /* XXX PAL */
		//tw_write(dev, R8_STANDARD_SEL(id), 0x0F);
		tw_write(dev, R8_STANDARD_SEL(id), 0x09); /* XXX PAL */
		tw_write(dev, R8_REG(0x107, id), 0x12); /* crop */
		tw_write(dev, R8_REG(0x10B, id), 0xd0); /* 720 0x2d0 */
		tw_write(dev, R8_REG(0x109, id), 0x20); /* 576/2 0x120 */
		//tw_write(dev, R8_VDELAY(id), 0x17);     /* VDelay */
		tw_write(dev, R8_VDELAY(id), 0x18);     /* VDelay */
		tw_write(dev, R8_REG(0x10A, id), 0x06); /* HDelay */
	}
  regDW = tw_read(dev, R32_PHASE_REF) & 0xFFFF0000u;
	tw_write(dev, R32_PHASE_REF, regDW | 0xAAAA0000u); 
	//tw_write(dev, R32_PHASE_REF, 0xAAAA144D);
	//tw_write(dev, R32_PHASE_REF, 0xAAAA1518);
	//tw_write(dev, VERTICAL_CTRL, 0x26); //0x26 will cause ch0 and ch1 have dma_error.  0x24
	tw_write(dev, R8_VERTICAL_CONTROL1(0), 0x22); // allow auto field generation as beforem, do nor activate DETV as recommended
	tw_write(dev, R8_VERTICAL_CONTROL1(4), 0x22);
	tw_write(dev, R8_MISC_CONTROL1(0), 0x16);
	tw_write(dev, R8_MISC_CONTROL1(4), 0x16);

	/* Reset Internal audio and video decoders */
	tw_write(dev, R8_AVSRST(0), 0x1F);
	tw_write(dev, R8_AVSRST(4), 0x1F);


	tw_write(dev, R32_DMA_CMD, 0);
	tw_read(dev, R32_DMA_CMD);

	tw_write(dev, R32_DMA_CHANNEL_ENABLE, 0);
	tw_read(dev, R32_DMA_CHANNEL_ENABLE);

	tw_write(dev, R32_DMA_CHANNEL_TIMEOUT, (0x3E << 24) | (0x7f0 << 12) | 0xff0);     // longer timeout setting
	regDW = tw_read(dev, R32_DMA_CHANNEL_TIMEOUT );
	dev_info(&dev->pdev->dev, "DMA: tw DMA_CHANNEL_TIMEOUT %x :: 0x%x\n", R32_DMA_CHANNEL_TIMEOUT, regDW);

	//tw_write(dev, R32_DMA_TIMER_INTERVAL, 0x1c000);
	//tw_write(dev, R32_DMA_TIMER_INTERVAL, 0);
	regDW = tw_read(dev, R32_DMA_TIMER_INTERVAL);
	dev_info(&dev->pdev->dev, "DMA: tw DMA_INT_REF %x :: 0x%x\n", R32_DMA_TIMER_INTERVAL, regDW);

	tw_write(dev, R32_DMA_CONFIG, 0xFFFFFF0C);
	tw_read(dev, R32_DMA_CONFIG);

	tw_write(dev, R32_VIDEO_CONTROL2, 0x00FF00FF);

#if 0 /* XXX not documented but in origin driver (RSR) */
	regDW = tw_read(dev, CSR_REG);
	regDW &= 0x7FFF;
	tw_write(dev, CSR_REG, regDW);
	mdelay(100);
	regDW |= 0x8002;  // Pull out from reset and enable I2S
	tw_write(dev, CSR_REG, regDW);
#endif

	/* Show black background if no signal */
	tw_write(dev, R8_MISC_CONTROL2(0), 0xE4);
	tw_write(dev, R8_MISC_CONTROL2(4), 0xE4);

	return 0;
}

static int tw6869_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
		struct tw6869_dev *dev;
		int ret;
		u32 regDW;

		/* Allocate a new instance */
		dev = devm_kzalloc(&pdev->dev, sizeof(struct tw6869_dev), GFP_KERNEL);
		if (!dev)
				return -ENOMEM;

		/* Enable PCI */
		ret = pci_enable_device(pdev);
		if (ret)
				return ret;

		pci_set_master(pdev);
		ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (ret) {
				dev_err(&pdev->dev, "no suitable DMA available\n");
				goto disable_pci;
		}

		/* Get mmio */
		ret = pci_request_regions(pdev, KBUILD_MODNAME);
		if (ret)
				goto disable_pci;

		dev->mmio = pci_iomap(pdev, 0, 0);
		if (!dev->mmio) {
				ret = -EIO;
				goto release_regs;
		}

#if TRACE_SIZE
		memset(&ramdp, 0x00, sizeof(ramdp));
		spin_lock_init(&ramdp.lock);
		ramdp.len = TRACE_SIZE;
		ramdp.pos = ramdp.buf;
		ramdp.proc_entry_cat = proc_create("tw_cat", 0, NULL, &ramdp_fops);
		if (NULL == ramdp.proc_entry_cat) {
				pr_info("Creating /proc/tw_cat failed.\n");
				ret = -ENODEV;
				goto release_regs;
		}
		ramdp.tracePos = 0;
#endif


		pci_read_config_dword(pdev, PCI_COMMAND, &regDW);	// 04 PCI_COMMAND
		regDW |= 7;
		regDW &= 0xfffffbff;
		pci_write_config_dword(pdev, PCI_COMMAND, regDW);


		// MSI CAP     disable MSI
		pci_read_config_dword(pdev, 0x50, &regDW);
		regDW &= 0xfffeffff;
		pci_write_config_dword(pdev, 0x50, regDW);

		//  MSIX  CAP    disable
		pci_read_config_dword(pdev, 0xac, &regDW);
		regDW &= 0x7fffffff;
		pci_write_config_dword(pdev, 0xac, regDW);

		pci_read_config_dword(pdev, 0x78, &regDW);
		regDW &= 0xfffffe1f;
		regDW |= (0x8 << 5);	///  8 - 128   ||  9 - 256  || A - 512
		pci_write_config_dword(pdev, 0x78, regDW);

		mdelay(20);
		dev->pdev = pdev;
		if (0 != tw6869_reset(dev)) { goto unmap_regs; }

#ifdef LAST_ENABLE
		dev->dma_last_enable = 0;
#endif /* LAST_ENABLE */

		init_timer(&dev->dma_resync);
		dev->dma_resync.function = dma_resync;
		dev->dma_resync.data = (unsigned long)dev;	///(unsigned long)(&dev);
		mod_timer(&dev->dma_resync, jiffies + msecs_to_jiffies(30));

		/* Allocate the interrupt */
		ret = devm_request_irq(&pdev->dev, pdev->irq,
						tw6869_irq, IRQF_SHARED, KBUILD_MODNAME, dev);
		if (ret) {
				dev_err(&pdev->dev, "request_irq failed\n");
				goto unmap_regs;
		}

		dev->ch_max = TW_CH_MAX;
		spin_lock_init(&dev->rlock);

		ret = tw6869_video_register(dev);
		if (ret)
				goto unmap_regs;

#ifdef IMPL_AUDIO
		ret = tw6869_audio_register(dev);
		if (ret)
				goto video_unreg;
#endif /* IMPL_AUDIO */

		ret = device_create_file(&pdev->dev, &dev_attr_tw_reg);
		if (ret < 0)
				dev_warn(&pdev->dev, "failed to add tw sysfs files\n");
		dev_info(&pdev->dev, "driver loaded %p\n", dev);
		return 0;

#ifdef IMPL_AUDIO
video_unreg:
		tw6869_video_unregister(dev);
#endif /* IMPL_AUDIO */
unmap_regs:
		pci_iounmap(pdev, dev->mmio);
release_regs:
		pci_release_regions(pdev);
disable_pci:
		pci_disable_device(pdev);
		return ret;
}

static void tw6869_remove(struct pci_dev *pdev)
{
	struct v4l2_device *v4l2_dev = pci_get_drvdata(pdev);
	struct tw6869_dev *dev =
		container_of(v4l2_dev, struct tw6869_dev, v4l2_dev);

	del_timer(&dev->dma_resync);
	device_remove_file(&dev->pdev->dev, &dev_attr_tw_reg);
	remove_proc_entry("tw_cat", NULL);
#ifdef IMPL_AUDIO
	tw6869_audio_unregister(dev);
#endif /* IMPL_AUDIO */
	tw6869_video_unregister(dev);
	pci_iounmap(pdev, dev->mmio);
	pci_release_regions(pdev);
	pci_disable_device(dev->pdev);
}

static struct pci_driver tw6869_driver = {
	.name = KBUILD_MODNAME,
	.probe = tw6869_probe,
	.remove = tw6869_remove,
	.id_table = tw6869_pci_tbl,
};

module_pci_driver(tw6869_driver);

MODULE_ALIAS("pci:v*00001797d00006869sv*sd*bc*sc*i*");
