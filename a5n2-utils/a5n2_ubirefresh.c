#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include <libubi.h>


#define PROGRAM_NAME "a5n2_refresh"


static const char *usage =
"Usage: " PROGRAM_NAME " <ubi_device>\n"
"\n"
"   e.g.: " PROGRAM_NAME " /dev/ubi0\n"
"\n"
"Safely migrates all LEBs of a UBI-formatted device to new PEBs. Outputs\n"
"human-readable progress to stdout, and returns 0 on success.\n";


struct stats_t {
	unsigned mapped;
	unsigned unmapped;
	unsigned exceptions;
};


static int refresh(char *node)
{
	int rc = 0;

	libubi_t libubi = libubi_open();
	if (!libubi) {
		fprintf(stderr, "can't initialize libubi\n");
		return EXIT_FAILURE;
	}

	rc = ubi_probe_node(libubi, node);
	if (rc != 1) {
		fprintf(stderr, "%s is not a UBI device node\n", node);
		return EXIT_FAILURE;
	}

	struct ubi_dev_info dev_info;
	rc = ubi_get_dev_info(libubi, node, &dev_info);
	if (rc) {
		fprintf(stderr, "can't read UBI device info for %s\n", node);
		return EXIT_FAILURE;
	}

	unsigned char *leb_data = malloc(dev_info.leb_size);

	for (int v = dev_info.lowest_vol_id; v <= dev_info.highest_vol_id; ++v) {
		char vol_node[80];
		snprintf(vol_node, 80, "%s_%d", node, v);
		struct ubi_vol_info vol_info;
		rc = ubi_get_vol_info(libubi, vol_node, &vol_info);
		if (rc) {
			fprintf(stderr, "can't read UBI volume info for %s\n", vol_node);
			return EXIT_FAILURE;
		}

		int vol_fd = open(vol_node, O_RDWR);
		if (vol_fd < 0) {
			fprintf(stderr, "can't open volume node %s: %s\n", vol_node, strerror(errno));
			return EXIT_FAILURE;
		}

		struct stats_t stats;
		memset(&stats, 0, sizeof(stats));

		for (int b = 0; b < vol_info.rsvd_lebs; ++b) {
			if (ubi_is_mapped(vol_fd, b)) {
				++stats.mapped;

				rc = lseek(vol_fd, b * vol_info.leb_size, SEEK_SET);
				if (rc == -1) {
					++stats.exceptions;
					continue;
				}

				rc = read(vol_fd, leb_data, vol_info.leb_size);
				if (rc != vol_info.leb_size) {
					/* TODO: handle EAGAIN? */
					++stats.exceptions;
					continue;
				}

				rc = ubi_leb_change_start(libubi, vol_fd, b, vol_info.leb_size);
				if (rc) {
					++stats.exceptions;
					continue;
				}

				rc = write(vol_fd, leb_data, vol_info.leb_size);
				if (rc != vol_info.leb_size) {
					/* TODO: handle EAGAIN? */
					/* TODO: how affect leb_change? */
					++stats.exceptions;
					continue;
				}
			} else {
				++stats.unmapped;
			}
		}

		printf("vol: nr=%d name=%s mapped=%d unmapped=%d exceptions=%d\n",
			vol_info.vol_id, vol_info.name, stats.mapped, stats.unmapped, stats.exceptions);

	}

	free(leb_data);

	return EXIT_SUCCESS;
}


int main(int argc, char *argv[])
{
	switch (argc) {
	case 2:
		if (strncmp("-h", argv[1], 2) == 0 ||
		    strncmp("--help", argv[1], 6) == 0 ) {
			printf(usage);
			return EXIT_SUCCESS;
		} else {
			return refresh(argv[1]);
		}

		break;

	default:
		fprintf(stderr, usage);
	}

	return EXIT_FAILURE;
}
