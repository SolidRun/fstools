/*
 * Copyright (C) 2014 John Crispin <blogic@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <asm/setup.h>

#include <libubox/ulog.h>

#include "libfstools/libfstools.h"
#include "libfstools/volume.h"

/*
 * Called in the early (PREINIT) stage, when we immediately need some writable
 * filesystem.
 */
static int
start(int argc, char *argv[1])
{
	struct volume *root;
	struct volume *data = NULL;
	struct stat s;

	if (!getenv("PREINIT") && stat("/tmp/.preinit", &s))
		return -1;

	/*
	 * Check cmdline for a hint about overlay device
	 * e.g. /dev/mmcblk0p3
	 */
	do {
		FILE *fp;
		char buffer[COMMAND_LINE_SIZE] = {0};

		fp = fopen("/proc/cmdline", "r");
        if(!fp) {
			ULOG_WARN("Failed to open /proc/cmdline for reading\n");
			break;
		}
		while (!feof(fp)) {
			if(fscanf(fp, "overlay=%s", buffer))
				break;

			fseek(fp, 1, SEEK_CUR);
		}
		fclose(fp);

		/*
		 * If an overlay= argument was found, look for a volume with that name
		 */
		if (buffer[0]) {
			/*
			 * strip /dev/ prefix if any
			 */
			int offset = 0;
			if (strstr(buffer, "/dev/"))
				offset = 5;

			ULOG_NOTE("Looking for overlay device given on commandline\n");
			data = volume_find(buffer + offset);
		}
	} while(0);

	/*
	 * Look for default rootfs_data partition name
	 */
	if(!data)
		data = volume_find("rootfs_data");

	/*
	 * In case rootfs_data doesn't exist, only mount /dev/root for now
	 */

	/*
	 * When the default overlay partition name rootfs_data can not be found,
	 * fall back to the special /dev/root device.
	 */
	if (!data) {
		root = volume_find("rootfs");
		volume_init(root);

		// mount /dev/root at /
		ULOG_NOTE("mounting /dev/root\n");
		mount("/dev/root", "/", NULL, MS_NOATIME | MS_REMOUNT, 0);

		/*
		 * Now that / has been mounted, and there is no overlay device,
		 * see if extroot is configured.
		 * 
		 * The following call will handle reading configuration from
		 * rootfs on its own.
		 */
		extroot_prefix = "";
		if (!mount_extroot()) {
			ULOG_NOTE("switched to extroot\n");
			/*
			 * extroot succeeded mounting an overlay partition, return.
			 */
			return 0;
		}

		/*
		 * Even if extroot was not configured, considering that no overlay
		 * partition was found, and / was mounted, return now.
		 */
		return 0;
	}

	/*
	 * neither /dev/root nor extroot were used.
	 * Attempt to mount the overlay partition.
	 */
	switch (volume_identify(data)) {
	case FS_NONE:
		ULOG_WARN("no usable overlay filesystem found, using tmpfs overlay\n");
		return ramoverlay();

	case FS_DEADCODE:
		/*
		 * Filesystem isn't ready yet and we are in the preinit, so we
		 * can't afford waiting for it. Use tmpfs for now and handle it
		 * properly in the "done" call.
		 */
		ULOG_NOTE("jffs2 not ready yet, using temporary tmpfs overlay\n");
		return ramoverlay();

	case FS_JFFS2:
	case FS_UBIFS:
	case FS_EXT4FS:
		mount_overlay(data);
		break;

	case FS_SNAPSHOT:
		mount_snapshot(data);
		break;
	}

	return 0;
}

static int
stop(int argc, char *argv[1])
{
	if (!getenv("SHUTDOWN"))
		return -1;

	return 0;
}

/*
 * Called at the end of init, it can wait for filesystem if needed.
 */
static int
done(int argc, char *argv[1])
{
	struct volume *v = NULL;\

	/*
	 * Check cmdline for a hint about overlay device
	 * e.g. /dev/mmcblk0p3
	 */
	do {
		FILE *fp;
		char buffer[COMMAND_LINE_SIZE] = {0};

		fp = fopen("/proc/cmdline", "r");
        if(!fp) {
			ULOG_WARN("Failed to open /proc/cmdline for reading\n");
			break;
		}
		while (!feof(fp)) {
			if(fscanf(fp, "overlay=%s", buffer))
				break;

			fseek(fp, 1, SEEK_CUR);
		}
		fclose(fp);

		/*
		 * If an overlay= argument was found, look for a volume with that name
		 */
		if (buffer[0]) {
			/*
			 * strip /dev/ prefix if any
			 */
			int offset = 0;
			if (strstr(buffer, "/dev/"))
				offset = 5;

			ULOG_NOTE("Looking for overlay device given on commandline\n");
			v = volume_find(buffer + offset);
		}
	} while(0);

	/*
	 * Look for default rootfs_data partition name
	 */
	if (!v)
		v = volume_find("rootfs_data");

	/*
	 * When no overlay partition is found there is nothing to do
	 */
	if (!v)
		return -1;

	switch (volume_identify(v)) {
	case FS_NONE:
	case FS_DEADCODE:
		return jffs2_switch(v);

	case FS_JFFS2:
	case FS_UBIFS:
	case FS_EXT4FS:
		fs_state_set("/overlay", FS_STATE_READY);
		break;
	}

	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return start(argc, argv);
	if (!strcmp(argv[1], "ram"))
		return ramoverlay();
	if (!strcmp(argv[1], "stop"))
		return stop(argc, argv);
	if (!strcmp(argv[1], "done"))
		return done(argc, argv);
	return -1;
}
