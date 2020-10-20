#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <getopt.h>
#include <stdbool.h>

#include <mtd/mtd-user.h>
#include <libmtd.h>

#define PROGRAM_NAME "shallowerasetest"
#include "common.h"
#undef round_down
#undef round_up

static const char* usage =
"Usage: " PROGRAM_NAME " [OPTIONS] <mtd_device> <range>\n"
"\n"
"The argument <range> is the range of pages or bytes to test\n"
"within each block, given in something like Python slice notation,\n"
"prefixed with 'p:' or 'b:' to indicate pages or bytes. E.g.:\n"
"\n"
"  p:0:4     the first four pages\n"
"  b:0:1024  bytes zero thru 1023\n"
"  p:-4:     the last four pages\n"
"\n"
"Options:\n"
"  -h, --help            Print this help.\n"
"  -n, --no-zero         Don't zero <range> prior to erase.\n"
"  -s, --skip-deep       Skip deeply erasing block before testing\n"
"  -b, --blocks <range>  The range of blocks to test (default: entire device)\n"
"  -w  --writes-per-page <1|2|4> make 1, 2, or 4 overlapping partial writes within page\n"
"  -o  --overlap         Overlap writes-per-page to create maximum program disturb\n"
"  -g  --group-size      Bytes to group together for reporting (default: 512)\n"
"  -c  --count           Stop after count blocks (default: never)\n"
"\n"
"  e.g.: " PROGRAM_NAME " --blocks 0:32 /dev/mtd2 p:0:2\n"
"\n"
"Try to induce and detect bit flips caused by a shallow erase, a\n"
"problem discovered on certain NAND chips whereby PEBs with relatively\n"
"few programmed bits don't fully erase, potentially leaving\n"
"behind weekly charged 1-bits that sometimes read as 0-bits,\n"
"especially when nearby bits are programmed.\n";

enum exit_status {
	SUCCESS = 0,
	FAILURE = 1,
	IOERROR = 2,
};

struct span {
    int begin;
    int end;
};

inline size_t span_len(struct span* s)
{
    return s->end - s->begin;
}

struct writer {
    libmtd_t libmtd;
    struct mtd_dev_info* info;
    int fd;
    uint8_t* pagebuf;
};

static enum exit_status
writer_init(struct writer* writer, libmtd_t libmtd, struct mtd_dev_info* info, int fd)
{
    writer->libmtd = libmtd;
    writer->info = info;
    writer->fd = fd;

    writer->pagebuf = malloc(info->min_io_size);
    if (!writer->pagebuf) {
        fprintf(stderr, "error: failed to allocate memory\n");
        return FAILURE;
    }

    return SUCCESS;
}

static void writer_free(struct writer* writer)
{
    if (writer->pagebuf) {
        free(writer->pagebuf);
        writer->pagebuf = 0;
    }
}

inline int round_down(int add, int size)
{
    return add - add % size;
}

inline int round_up(int add, int size)
{
    if (add % size) {
        return add + size - add % size;
    } else {
        return add;
    }
}

static enum exit_status
writer_write_fill(struct writer* writer, int eb, struct span* bytes, uint8_t val)
{
    const int page_size = writer->info->min_io_size;

    for (int byte = round_down(bytes->begin, page_size);
         byte < round_up(bytes->end, page_size);
         ++byte)
    {
        if (byte >= bytes->begin && byte < bytes->end) {
            writer->pagebuf[byte % page_size] = val;
        } else {
            writer->pagebuf[byte % page_size] = 0xff;
        }

        if ((byte + 1) % page_size == 0) { // last byte in page
            int err = mtd_write(writer->libmtd, writer->info, writer->fd, eb,
                    round_down(byte, page_size), writer->pagebuf, page_size, 0, 0, 0);
            if (err) {
                return FAILURE;
            }
        }
    }

    return SUCCESS;
}

struct reader {
    libmtd_t libmtd;
    struct mtd_dev_info* info;
    int fd;
    unsigned char* blockbuf;
};

static enum exit_status
reader_init(struct reader* reader, libmtd_t libmtd, struct mtd_dev_info* info, int fd)
{
    reader->libmtd = libmtd;
    reader->info = info;
    reader->fd = fd;

    reader->blockbuf = malloc(info->eb_size);
    if (!reader->blockbuf) {
        fprintf(stderr, "error: failed to allocate memory\n");
        return FAILURE;
    }

    return SUCCESS;
}

static void reader_free(struct reader* reader)
{
    if (reader->blockbuf) {
        free(reader->blockbuf);
        reader->blockbuf = 0;
    }
}

static enum exit_status reader_read(struct reader* reader, int eb, struct span* span)
{
    int err = mtd_read(reader->info, reader->fd, eb, span->begin,
            reader->blockbuf + span->begin, span_len(span));
    if (err) {
        fprintf(stderr, "error: failed to read PEB %d\n", eb);
        return IOERROR;
    }

    return SUCCESS;
}

static volatile int exit_flag = 0;

static void set_exit_flag(int signal)
{
    switch(signal) {
    default:
        exit_flag = 1;
    }
}

static enum exit_status parse_span(const char* range, struct span* span, int end)
{
    char* next = 0;

    if (range == 0) {
        return FAILURE;
    }

    switch(*range) {
    case ':':
        span->begin = 0;
        ++range;

        if (*range == '\0') { // range: ":"
            span->end = end;
            return SUCCESS;
        } else { // range ":#"
            span->end = strtol(range, &next, 0);
            if (next == range) {
                return FAILURE;
            } else {
                range = next;
            }
        }

        break;

    default: // range: "#:?"
        span->begin = strtol(range, &next, 0);
        if (next == range) {
            return FAILURE;
        } else {
            range = next;
        }

        if (*range++ != ':') {
            return FAILURE;
        }

        if (*range == '\0') {
            span->end = end;
            break;
        }

        span->end = strtol(range, &next, 0); 
        if (next == range) {
            return FAILURE;
        }
    }

    if (span->begin < 0) {
        span->begin += end;
    }

    if (span->end < 0) {
        span->end += end;
    }

    if (span->begin <= span->end) {
        return SUCCESS;
    } else {
        return FAILURE;
    }
}

static enum exit_status parse_range(const char* range, struct span* span, struct mtd_dev_info* info)
{
    if (range == 0) {
        return FAILURE;
    }

    int rc = FAILURE;
    if (!strncmp(range, "p:", 2)) {
        rc = parse_span(range + 2, span, info->eb_size / info->min_io_size);
        span->begin *= info->min_io_size;
        span->end *= info->min_io_size;
    } else if (!strncmp(range, "b:", 2)) {
        rc = parse_span(range + 2, span, info->eb_size);
    } else { // default to byte units
        rc = parse_span(range, span, info->eb_size);
    }

    return rc;
}

static enum exit_status
write_pattern(struct writer* writer, int eb, struct span* bytes, int writes_per_page, bool overlap)
{
    enum exit_status rc;

    switch (writes_per_page) {
    case 1:
        rc = writer_write_fill(writer, eb, bytes, 0xaa);
        break;
    case 2:
        if (overlap) {
            rc = writer_write_fill(writer, eb, bytes, 0xee);
            if (rc != SUCCESS)
                break;
            rc = writer_write_fill(writer, eb, bytes, 0xbb);
        } else {
            struct span s1 = {bytes->begin, bytes->begin + span_len(bytes) / 2};
            rc = writer_write_fill(writer, eb, &s1, 0xaa);
            if (rc != SUCCESS)
                break;
            struct span s2 = {s1.end, s1.end + span_len(bytes) / 2};
            rc = writer_write_fill(writer, eb, &s2, 0xaa);
        }
        break;
    case 4:
        if (overlap) {
            rc = writer_write_fill(writer, eb, bytes, 0xfe);
            if (rc != SUCCESS)
                break;
            rc = writer_write_fill(writer, eb, bytes, 0xef);
            if (rc != SUCCESS)
                break;
            rc = writer_write_fill(writer, eb, bytes, 0xfb);
            if (rc != SUCCESS)
                break;
            rc = writer_write_fill(writer, eb, bytes, 0xbf);
        } else {
            struct span s1 = {bytes->begin, bytes->begin + span_len(bytes) / 4};
            rc = writer_write_fill(writer, eb, &s1, 0xaa);
            if (rc != SUCCESS)
                break;
            struct span s2 = {s1.end, s1.end + span_len(bytes) / 4};
            rc = writer_write_fill(writer, eb, &s2, 0xaa);
            if (rc != SUCCESS)
                break;
            struct span s3 = {s2.end, s2.end + span_len(bytes) / 4};
            rc = writer_write_fill(writer, eb, &s3, 0xaa);
            if (rc != SUCCESS)
                break;
            struct span s4 = {s3.end, s3.end + span_len(bytes) / 4};
            rc = writer_write_fill(writer, eb, &s4, 0xaa);
        }
        break;
    default:
        rc = FAILURE;
    }

    return rc;
}

struct histogram {
    unsigned long long flips[1 + 16];
    unsigned long long overflow;
    unsigned long long bad;
    unsigned long long blocks;
};

static void histogram_reset(struct histogram* h)
{
    memset(h, 0, sizeof(*h));
}

static void histogram_print(struct histogram* h)
{
    printf("FLIPS: GROUPS\n");
    for(int i = 0; i < ARRAY_SIZE(h->flips); ++i) {
        printf("%3d: %llu\n", i, h->flips[i]);
    }
    printf(">16: %llu\n", h->overflow);
    printf("bad blocks: %llu\n", h->bad);
    printf("total blocks: %llu\n", h->blocks);
}

static unsigned count_ones(unsigned byte)
{
    int count;
    for (count = 0; byte != 0; ++count) {
        byte &= byte - 1; // The Brian Kernighan method; clears least significant set bit.
    }

    return count;
}

struct params {
    const char* nodename;      // filename of MTD node
    bool zero;                 // use zero for pre-erase pattern
    bool skip_deep;            // skip deeply erasing block before test
    bool overlap;              // overlap writes_per_page for maximum program distrub
    const char* blockspec;     // range of blocks to test
    const char* rangespec;     // range within each block to test
    unsigned writes_per_page;  
    bool write_entire_block;   // write test pattern to entire block, not just test range
    unsigned group_size;       // bytes to group together for reporting
    unsigned count;            // stop after count blocks (if zero don't stop)
};

static int test(struct params* params)
{
	int rc = 0;
    libmtd_t libmtd = 0;
    int fd = 0;
    struct writer writer;
    memset(&writer, 0, sizeof(writer));
    struct reader reader;
    memset(&reader, 0, sizeof(reader));
    struct histogram histogram;
    histogram_reset(&histogram);

	libmtd = libmtd_open();
	if (!libmtd) {
		fprintf(stderr, "can't initialize libmtd\n");
		rc = FAILURE;
        goto finish;
	}

    fd = open(params->nodename, O_RDWR);
    if (!fd) {
		fprintf(stderr, "can't open %s: %s\n", params->nodename, strerror(errno));
        rc = FAILURE;
        goto finish;
    }

    rc = ioctl(fd, MTDFILEMODE, MTD_FILE_MODE_RAW);
    if (rc) {
        perror("error: can't set MTD_FILE_MODE_RAW");
        rc = FAILURE;
        goto finish;
    }

    struct mtd_dev_info info;
    rc = mtd_get_dev_info(libmtd, params->nodename, &info);
    if (rc < 0) {
		fprintf(stderr, "can't read mtd info: %s\n", strerror(errno));
        rc = FAILURE;
        goto finish;
    }

	if (info.type != MTD_NANDFLASH) {
		fprintf(stderr, "error: not a NAND device\n");
		rc = FAILURE;
        goto finish;
	}

    struct span bytes;
    rc = parse_range(params->rangespec, &bytes, &info);
    if (rc != SUCCESS) {
		fprintf(stderr, "error: invalid page or byte range\n");
        goto finish;
    }

    struct span blocks;
    if (params->blockspec) {
        rc = parse_span(params->blockspec, &blocks, info.eb_cnt);
        if (rc != SUCCESS) {
            fprintf(stderr, "error: invalid block range\n");
            goto finish;
        }
    } else {
        blocks.begin = 0;
        blocks.end = info.eb_cnt;
    }

    printf("testing b:%d:%d at p:%d[%d]:%d[%d]\n",
            blocks.begin, blocks.end,
            bytes.begin / info.min_io_size, bytes.begin % info.min_io_size,
            bytes.end / info.min_io_size, bytes.end % info.min_io_size);

    rc = writer_init(&writer, libmtd, &info, fd);
    if (rc != SUCCESS)
        goto finish;

    rc = reader_init(&reader, libmtd, &info, fd);
    if (rc != SUCCESS)
        goto finish;

    signal(SIGINT, set_exit_flag);

    /* For each block in the range */
    for (int eb = blocks.begin;
        !exit_flag && (params->count == 0 || (histogram.blocks < params->count));
        eb = (eb >= blocks.end - 1) ? blocks.begin : eb + 1)
    {

         /* Skip bad blocks */
		rc = mtd_is_bad(&info, fd, eb);
		if (rc == 1) {
            ++histogram.bad;
			continue;
		} else if (rc == -1) {
            fprintf(stderr, "error: reading bad-block state of PEB %d\n", eb);
            rc = IOERROR;
            goto finish;
        }

        /* Zero and erase the block to establish a clean block. */
        if (!params->skip_deep) {
            struct span entire = {0, info.eb_size};
            rc = writer_write_fill(&writer, eb, &entire, 0x00);
            if (rc != SUCCESS) {
                fprintf(stderr, "error: initially zeroing PEB %d\n", eb);
                rc = IOERROR;
                goto finish;
            }
        }

        rc = mtd_erase(libmtd, &info, fd, eb);
        if (rc) {
            fprintf(stderr, "error: initially erasing PEB %d\n", eb);
            rc = IOERROR;
            goto finish;
        }

        /* Write pre-erase data */
        if (params->zero) {
            rc = writer_write_fill(&writer, eb, &bytes, 0x00);
        } else {
            rc = writer_write_fill(&writer, eb, &bytes, 0x55);
        }
        if (rc != SUCCESS) {
            fprintf(stderr, "error: writing pre-erase pattern to PEB %d\n", eb);
            goto finish;
        }

        /* Erase the block. */
        rc = mtd_erase(libmtd, &info, fd, eb);
        if (rc) {
            fprintf(stderr, "error: erasing PEB %d\n", eb);
            rc = IOERROR;
            goto finish;
        }

        /* Write test pattern */
        struct span write_bytes;
        if (params->write_entire_block) {
            write_bytes.begin = 0;
            write_bytes.end = info.eb_size;
        } else {
            write_bytes = bytes;
        }
        rc = write_pattern(&writer, eb, &write_bytes, params->writes_per_page, params->overlap);
        if (rc != SUCCESS) {
            fprintf(stderr, "error: writing to PEB %d\n", eb);
            goto finish;
        }

        /* Read back the range and report any flipped bits */
        rc = reader_read(&reader, eb, &bytes);
        if (rc != SUCCESS) {
            fprintf(stderr, "error: reading PEB %d\n", eb);
            goto finish;
        }

        for (unsigned byte = bytes.begin, flipped = 0; byte < bytes.end; ++byte) {
            flipped += count_ones(0xaa ^ reader.blockbuf[byte]);

            if ((byte + 1) % params->group_size == 0) {
                if (flipped <= 16) {
                    histogram.flips[flipped] += 1;
                } else {
                    histogram.overflow += 1;
                }

                if (flipped > 0) {
                    printf("%llu: %d bit-flips in b:%d[%d][0x%x:0x%x]\n",
                           histogram.blocks,
                           flipped,
                           eb,
                           byte / info.min_io_size,
                           (byte - params->group_size + 1) % info.min_io_size,
                           (byte - params->group_size + 1) % info.min_io_size + params->group_size);
                }

                flipped = 0;
            }
        }

        if (++histogram.blocks % 1024 == 0) {
            printf("%llu: starting block %d\n", histogram.blocks, eb);
        }
    }

    histogram_print(&histogram);

finish:
    reader_free(&reader);
    writer_free(&writer);
    if (fd) close(fd);
    if (libmtd) libmtd_close(libmtd);

    return rc;
}

static const struct option options[] = {
    {"help", no_argument, 0, 'h'},
    {"no-zero", no_argument, 0, 'n'},
    {"overlap", no_argument, 0, 'o'},
    {"write-entire-block", no_argument, 0, 'e'},
    {"blocks", required_argument, 0, 'b'},
    {"writes-per-page", required_argument, 0, 'w'},
    {"group-size", required_argument, 0, 'g'},
    {"skip-deep", no_argument, 0, 's'},
    {"count", required_argument, 0, 'c'},
    {0, 0, 0, 0},
};

int main(int argc, char** argv)
{
    int c;
    struct params params;
    memset(&params, 0, sizeof(params));
    params.zero = true;
    params.writes_per_page = 1;
    params.group_size = 512;

    while(1) {
        c = getopt_long(argc, argv, "noebs:w:g:c:h", options, 0);
        if (c == -1)
            break;

        switch (c) {
            case 'n':
                params.zero = false;
                break;
            case 'b':
                params.blockspec = optarg;
                break;
            case 'e':
                params.write_entire_block = true;
                break;
            case 's':
                params.skip_deep = true;
                break;
            case 'w':
                errno = 0;
                params.writes_per_page = strtol(optarg, 0, 0);
                if (errno) {
                    fputs("error: bad argument to --writes-per-page\n", stderr);
                    fputs(usage, stderr);
                    return EXIT_FAILURE;
                }
                if (params.writes_per_page != 1 
                && params.writes_per_page != 2
                && params.writes_per_page != 4) {
                    fputs("error: must only write 1, 2, or 4 times\n", stderr);
                    return EXIT_FAILURE;
                }
                break;
            case 'g':
                params.group_size = strtol(optarg, 0, 0);
                if (errno) {
                    fputs("error: bad argument to --group-size\n", stderr);
                    fputs(usage, stderr);
                    return EXIT_FAILURE;
                }
                break;
            case 'o':
                params.overlap = true;
                break;
            case 'c':
                params.count = strtol(optarg, 0, 0);
                if (errno) {
                    fputs("error: bad argument to --count\n", stderr);
                    fputs(usage, stderr);
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
                fputs(usage, stdout);
                return EXIT_SUCCESS;
            default:
                fputs(usage, stderr);
                return EXIT_FAILURE;
        }
    }

    if (optind < argc - 1) {
        params.nodename = argv[optind++];
        params.rangespec = argv[optind];
    } else {
        fputs(usage, stderr);
        return EXIT_FAILURE;
    }

    return test(&params);
}
