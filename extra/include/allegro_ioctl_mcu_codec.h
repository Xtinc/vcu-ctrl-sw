// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#ifndef _AL_CODEC_IOCTL_H_
#define _AL_CODEC_IOCTL_H_

#include <linux/types.h>

struct al5_dma_info
{
	__u32 fd;
	__u32 size;
	/* this should disappear when the last use of phy addr is removed from
	 * userspace code */
	__u32 phy_addr;
};

#define OPAQUE_SIZE 128

struct al5_channel_status
{
	__u32 error_code;
};

struct al5_params
{
	__u32 size;
	__u32 opaque[OPAQUE_SIZE];
};

#endif	/* _AL_CODEC_IOCTL_H_ */
