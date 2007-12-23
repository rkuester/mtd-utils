/*
 * Copyright (c) Nokia Corporation, 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Author: Artem B. Bityutskiy
 *
 * Test volume reference counting - create a volume, open a sysfs file
 * belonging to the volume, delete the volume but do not close the file, make
 * sure the file cannot be read, make sure the volume cannot be open, close the
 * file, make sure the volume disappeard, make sure its sysfs subtree
 * disappeared.
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "libubi.h"
#define TESTNAME "rmvol"
#include "common.h"

#define SYSFS_FILE "/sys/class/ubi/ubi%d_%d/usable_leb_size"

int main(int argc, char * const argv[])
{
	int ret, fd;
	char fname[sizeof(SYSFS_FILE) + 20];
	const char *node;
	libubi_t libubi;
	struct ubi_dev_info dev_info;
	struct ubi_mkvol_request req;
	char tmp[100];

	if (initial_check(argc, argv))
		return 1;

	node = argv[1];

	libubi = libubi_open();
	if (libubi == NULL) {
		failed("libubi_open");
		return 1;
	}

	if (ubi_get_dev_info(libubi, node, &dev_info)) {
		failed("ubi_get_dev_info");
		goto out_libubi;
	}

	/* Create a small dynamic volume */
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = dev_info.min_io_size;
	req.bytes = dev_info.leb_size;
	req.vol_type = UBI_DYNAMIC_VOLUME;
	req.name = "rmvol";

	if (ubi_mkvol(libubi, node, &req)) {
		failed("ubi_mkvol");
		perror("ubi_mkvol");
		goto out_libubi;
	}

	/* Open volume-related sysfs file */
	sprintf(fname, SYSFS_FILE, dev_info.dev_num, req.vol_id);
	fd = open(fname, O_RDONLY);
	if (fd == -1) {
		failed("open");
		perror("open");
		goto out_rmvol;
	}

	/* Remove the volume, but do not close the file */
	if (ubi_rmvol(libubi, node, req.vol_id)) {
		failed("ubi_rmvol");
		perror("ubi_rmvol");
		goto out_close;
	}

	/* Try to read from the file, this should fail */
	ret = read(fd, tmp, 100);
	if (ret != -1) {
		failed("read");
		err_msg("read returned %d, expected -1", ret);
		goto out_close;
	}

	/* Close the file and try to open it again, should fail */
	close(fd);
	fd = open(fname, O_RDONLY);
	if (fd != -1) {
		failed("open");
		err_msg("opened %s again, open returned %d, expected -1",
			fname, fd);
		goto out_libubi;
	}

	libubi_close(libubi);
	return 0;

out_rmvol:
	ubi_rmvol(libubi, node, req.vol_id);
out_libubi:
	libubi_close(libubi);
	return 1;

out_close:
	close(fd);
	libubi_close(libubi);
	return 1;
}

#if 0
/**
 * mkvol_alignment - create volumes with different alignments.
 *
 * Thus function returns %0 in case of success and %-1 in case of failure.
 */
static int mkvol_alignment(void)
{
	struct ubi_mkvol_request req;
	int i, vol_id, ebsz;
	const char *name = TESTNAME ":mkvol_alignment()";
	int alignments[] = ALIGNMENTS(dev_info.leb_size);

	for (i = 0; i < sizeof(alignments)/sizeof(int); i++) {
		req.vol_id = UBI_VOL_NUM_AUTO;

		/* Alignment should actually be multiple of min. I/O size */
		req.alignment = alignments[i];
		req.alignment -= req.alignment % dev_info.min_io_size;
		if (req.alignment == 0)
			req.alignment = dev_info.min_io_size;

		/* Bear in mind alignment reduces EB size */
		ebsz = dev_info.leb_size - dev_info.leb_size % req.alignment;
		req.bytes = dev_info.avail_lebs * ebsz;

		req.vol_type = UBI_DYNAMIC_VOLUME;
		req.name = name;

		if (ubi_mkvol(libubi, node, &req)) {
			failed("ubi_mkvol");
			err_msg("alignment %d", req.alignment);
			return -1;
		}

		vol_id = req.vol_id;
		if (check_volume(vol_id, &req))
			goto remove;

		if (ubi_rmvol(libubi, node, vol_id)) {
			failed("ubi_rmvol");
			return -1;
		}
	}

	return 0;

remove:
	ubi_rmvol(libubi, node, vol_id);
	return -1;
}

/**
 * mkvol_basic - simple test that checks basic volume creation capability.
 *
 * Thus function returns %0 in case of success and %-1 in case of failure.
 */
static int mkvol_basic(void)
{
	struct ubi_mkvol_request req;
	struct ubi_vol_info vol_info;
	int vol_id, ret;
	const char *name = TESTNAME ":mkvol_basic()";

	/* Create dynamic volume of maximum size */
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = dev_info.avail_bytes;
	req.vol_type = UBI_DYNAMIC_VOLUME;
	req.name = name;

	if (ubi_mkvol(libubi, node, &req)) {
		failed("ubi_mkvol");
		return -1;
	}

	vol_id = req.vol_id;
	if (check_volume(vol_id, &req))
		goto remove;

	if (ubi_rmvol(libubi, node, vol_id)) {
		failed("ubi_rmvol");
		return -1;
	}

	/* Create static volume of maximum size */
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = dev_info.avail_bytes;
	req.vol_type = UBI_STATIC_VOLUME;
	req.name = name;

	if (ubi_mkvol(libubi, node, &req)) {
		failed("ubi_mkvol");
		return -1;
	}

	vol_id = req.vol_id;
	if (check_volume(vol_id, &req))
		goto remove;

	if (ubi_rmvol(libubi, node, vol_id)) {
		failed("ubi_rmvol");
		return -1;
	}

	/* Make sure volume does not exist */
	ret = ubi_get_vol_info1(libubi, dev_info.dev_num, vol_id, &vol_info);
	if (ret == 0) {
		err_msg("removed volume %d exists", vol_id);
		goto remove;
	}

	return 0;

remove:
	ubi_rmvol(libubi, node, vol_id);
	return -1;
}

/**
 * mkvol_multiple - test multiple volumes creation
 *
 * Thus function returns %0 if the test passed and %-1 if not.
 */
static int mkvol_multiple(void)
{
	struct ubi_mkvol_request req;
	int i, ret, max = dev_info.max_vol_count;
	const char *name = TESTNAME ":mkvol_multiple()";

	/* Create maximum number of volumes */
	for (i = 0; i < max; i++) {
		char nm[strlen(name) + 50];

		req.vol_id = UBI_VOL_NUM_AUTO;
		req.alignment = 1;
		req.bytes = 1;
		req.vol_type = UBI_STATIC_VOLUME;

		sprintf(&nm[0], "%s:%d", name, i);
		req.name = &nm[0];

		if (ubi_mkvol(libubi, node, &req)) {
			if (errno == ENFILE) {
				max = i;
				break;
			}
			failed("ubi_mkvol");
			err_msg("vol_id %d", i);
			goto remove;
		}

		if (check_volume(req.vol_id, &req)) {
			err_msg("vol_id %d", i);
			goto remove;
		}
	}

	for (i = 0; i < max; i++) {
		struct ubi_vol_info vol_info;

		if (ubi_rmvol(libubi, node, i)) {
			failed("ubi_rmvol");
			return -1;
		}

		/* Make sure volume does not exist */
		ret = ubi_get_vol_info1(libubi, dev_info.dev_num, i, &vol_info);
		if (ret == 0) {
			err_msg("removed volume %d exists", i);
			goto remove;
		}
	}

	return 0;

remove:
	for (i = 0; i < dev_info.max_vol_count + 1; i++)
		ubi_rmvol(libubi, node, i);
	return -1;
}
#endif
