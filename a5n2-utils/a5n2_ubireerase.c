#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include <mtd/mtd-user.h>
#include <libmtd.h>
#include <libubi.h>


#define PROGRAM_NAME "a5n2_reerase"


static const char *usage =
"Usage: " PROGRAM_NAME " <mtd_device>\n"
"\n"
"   e.g.: " PROGRAM_NAME " /dev/mtd7\n"
"\n"
"Re-erase all unused PEBs of a UBI-formatted device. Outputs human-readable\n"
"progress to stdout, and returns 0 on success.\n";


struct stats_t {
	unsigned bad;
	unsigned failed; /* operations to NAND */
	unsigned mapped;
	unsigned unmapped;
	unsigned exceptions;
};


static int is_mapped(const struct mtd_dev_info *mtd, unsigned char *headers)
{
	return headers[mtd->subpage_size] != 0xff;
}


static int reerase(char *node)
{
	int rc = 0;

	libmtd_t libmtd = libmtd_open();
	if (!libmtd) {
		fprintf(stderr, "can't initialize libmtd\n");
		return EXIT_FAILURE;
	}

	int mtd_fd = open(node, O_RDWR);
	if (mtd_fd < 0) {
		fprintf(stderr, "can't open %s: %s\n", node, strerror(errno));
		return EXIT_FAILURE;
	}

	struct mtd_dev_info mtd_info;
	rc = mtd_get_dev_info(libmtd, node, &mtd_info);
	if (rc < 0) {
		fprintf(stderr, "can't read mtd info: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	if (mtd_info.type != MTD_NANDFLASH) {
		fprintf(stderr, "error: not a NAND device!\n");
		return EXIT_FAILURE;
	}

	libubi_t libubi = libubi_open();
	if (!libubi) {
		fprintf(stderr, "can't initialize libubi\n");
		return EXIT_FAILURE;
	}

	int ubi_num = -1;
	mtd_num2ubi_dev(libubi, mtd_info.mtd_num, &ubi_num);
	if (ubi_num != -1) {
		fprintf(stderr, "error: %s is attached\n", node);
		return EXIT_FAILURE;
	}

	unsigned char *headers = malloc(mtd_info.subpage_size + 1);
	struct stats_t stats;
	memset(&stats, 0, sizeof(stats));

	for (int b = 0; b < mtd_info.eb_cnt; ++b) {
		/* Skip bad blocks */
		rc = mtd_is_bad(&mtd_info, mtd_fd, b);
		if (rc == 1) {
			++stats.bad;
			continue;
		} else if (rc == -1) {
			++stats.failed;
		} else if (rc != 0) {
			++stats.exceptions;
		}

		/* Read erase header and first byte of volume header */
		rc = mtd_read(&mtd_info, mtd_fd, b, 0,
			headers, mtd_info.subpage_size + 1);
		if (rc != 0) {
			++stats.failed;
			continue;
		}

		if (is_mapped(&mtd_info, headers)) {
			/* Block is mapped to volume, dont' touch! */
			++stats.mapped;
		} else {
			/* Block is unmapped, erase and rewrite erase header */
			++stats.unmapped;
			rc = mtd_erase(libmtd, &mtd_info, mtd_fd, b);
			if (rc) {
				++stats.failed;
				continue;
			}

			rc = mtd_write(libmtd, &mtd_info, mtd_fd, b, 0,
				headers, mtd_info.subpage_size, 0, 0, 0);
			if (rc) {
				++stats.failed;
				continue;
			}
		}
	}

	free(headers);

	printf(PROGRAM_NAME ": bad=%d failed=%d mapped=%d unmapped=%d exceptions=%d\n",
		stats.bad, stats.failed, stats.mapped, stats.unmapped, stats.exceptions);

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
			return reerase(argv[1]);
		}

		break;

	default:
		fprintf(stderr, usage);
	}

	return EXIT_FAILURE;
}
