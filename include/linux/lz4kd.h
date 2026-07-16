/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_LZ4KD_H
#define _LINUX_LZ4KD_H

enum lz4kd_status {
	LZ4K_STATUS_INCOMPRESSIBLE = 0,
	LZ4K_STATUS_FAILED = -1,
	LZ4K_STATUS_READ_ERROR = -2,
	LZ4K_STATUS_WRITE_ERROR = -3,
};

const char *lz4kd_version(void);
unsigned int lz4kd_encode_state_bytes_min(void);
int lz4kd_encode(void *state, const void *in, void *out,
		 unsigned int in_max, unsigned int out_max,
		 unsigned int out_limit);
int lz4kd_decode(const void *in, void *out,
		 unsigned int in_max, unsigned int out_max);

#endif
