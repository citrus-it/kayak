/*
 * {{{ CDDL HEADER
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * }}}
 */

/* Copyright 2018 OmniOS Community Edition (OmniOSce) Association. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <ctype.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <inttypes.h>

#include <libnvpair.h>
#include <sys/queue.h>
#include <sys/sysmacros.h>

#include <zfsimpl.h>
#include <sha256.c>

#define XEN_PHYS "/xpvd/xdf@51712:a"
#define XEN_PATH "/dev/dsk/c2t0d0s0"

/* In libzpool */
extern size_t lz4_compress(void *src, void *dst, size_t s_len, size_t d_len,
    int level);
extern int lz4_decompress(void *src, void *dst, size_t s_len, size_t d_len,
    int level);

short verbose = 0;

void
hexdump(char *s, uint64_t len)
{
	uint64_t offset;

        for (offset = 0; offset < len; offset += 16)
        {
                uint64_t i;

                printf("%08lx    ", offset);

                for (i = offset; i - offset < 16; i++)
                {
                        if (i < len)
                                printf("%02x", s[i] & 0xff);
                        else
                                printf("  ");
                }

                printf("    ");

                for (i = offset; i < len && i - offset < 16; i++)
                        printf("%c", isprint((int)s[i]) ? s[i] : '.');

                printf("\n");
        }
	printf("\n");
}

void
zread(int fd, void *dst, uint64_t size, uint64_t offset)
{
	printf("Read from %p - %lu\n", offset, size);
	assert(pread64(fd, dst, size, offset + VDEV_LABEL_START_SIZE) == size);
}

void
_bpread(int fd, blkptr_t *bp, void **dst, uint64_t *dsize)
{
	const dva_t *dva = &bp->blk_dva[0];
	uint64_t size, offset;
	char *buf;

	// We only support simple cases
	assert(BP_IS_EMBEDDED(bp) == 0);
	assert(BP_GET_COMPRESS(bp) == ZIO_COMPRESS_LZ4);

	offset = DVA_GET_OFFSET(dva);
	size = BP_GET_PSIZE(bp);

	buf = malloc(size);
	zread(fd, buf, size, offset);

	*dst = buf;
	if (dsize)
		*dsize = size;
}

void
bpread(int fd, blkptr_t *bp, void *dst, uint64_t dsize)
{
	void *buf;
	uint64_t size;
	int rc;

	_bpread(fd, bp, &buf, &size);

	/* Decompress the data */
	rc = lz4_decompress(buf, dst, size, dsize, size);
	printf("LZ4 rc: %d\n", rc);
	assert(rc == 0);

	free(buf);
}

void
dn_read(int fd, dnode_phys_t *dnode, uint64_t offset, void *buf,
    uint64_t buflen)
{
	char *dnode_buf;
	int ibshift = dnode->dn_indblkshift - SPA_BLKPTRSHIFT;
	int bsize = dnode->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	int nlevels = dnode->dn_nlevels;
	int i, rc;

	printf("HERE with bsize=%d\n", bsize);

	dnode_buf = malloc(SPA_MAXBLOCKSIZE);

	while (buflen > 0) {
		uint64_t bn = offset / bsize;
		int boff = offset % bsize;
		int ibn;
		const blkptr_t *indbp;
		blkptr_t bp;

		indbp = dnode->dn_blkptr;
		for (i = 0; i < nlevels; i++) {
			ibn = bn >> ((nlevels - i - 1) * ibshift);
			ibn &= ((1 << ibshift) - 1);
			bp = indbp[ibn];
			if (BP_IS_HOLE(&bp)) {
				memset(dnode_buf, 0, bsize);
				break;
			}
			bpread(fd, &bp, dnode_buf, sizeof(dnode_buf));
			indbp = (const blkptr_t *) dnode_buf;
		}

		i = bsize - boff;
		if (i > buflen) i = buflen;
		memcpy(buf, &dnode_buf[boff], i);
		buf = ((char*) buf) + i;
		offset += i;
		buflen -= i;
	}

	free(dnode_buf);
}

uint64_t
label_offset(uint64_t size, int label)
{
	return label * sizeof(vdev_label_t) +
	    (label < VDEV_LABELS / 2 ? 0 :
	    size - VDEV_LABELS * sizeof(vdev_label_t));
}

void
update_vdev_labels(int fd, uint64_t size)
{
	vdev_label_t vl;
	nvlist_t *config;
	nvlist_t *vdt;
	vdev_phys_t *phys;
	zio_cksum_t ck;
	char *buf, *s;
	size_t buflen;
	int label, i;
	uint64_t ashift, txg, offset;

	/*
	 * Read the first VDEV label from the disk...
	 * There is no support here for reading a different label if the
	 * first is corrupt.
	 *	typedef struct vdev_label {
	 *	char		vl_pad1[VDEV_PAD_SIZE];
	 *	char		vl_pad2[VDEV_PAD_SIZE];
	 *	vdev_phys_t	vl_vdev_phys;
	 *	char		vl_uberblock[VDEV_UBERBLOCK_RING];
	 *	} vdev_label_t;
	 */
	assert(pread64(fd, &vl, sizeof(vdev_label_t), 0)
	    == sizeof(vdev_label_t));
	printf("Loaded label from disk\n");

	/*
	 * typedef struct vdev_phys {
	 *	char		vp_nvlist[VDEV_PHYS_SIZE - sizeof (zio_eck_t)];
	 *	zio_eck_t	vp_zbt;
	 *	} vdev_phys_t;
	 */
	phys = &vl.vl_vdev_phys;

	/* Validate the checksum */
	zio_checksum_SHA256(phys, VDEV_PHYS_SIZE, NULL, &ck);
	assert(ZIO_CHECKSUM_EQUAL(ck, phys->vp_zbt.zec_cksum) == 0);
	printf("Validated checksum\n");

	/* ..and unpack the nvlist */
	assert(nvlist_unpack(phys->vp_nvlist, VDEV_PHYS_SIZE, &config, 0) == 0);
	printf("Unpacked nvlist\n");

	/*
	 * The nvlist is a set of name/value pairs. Some of the values are
	 * nvlists themselves. Here's the start of the output from
	 * dump_nvlist(config, 8)
	 *
	 *	version: 5000
	 *	name: 'syspool'
	 *	vdev_children: 1
	 *	vdev_tree:
	 *	    type: 'disk'
	 *	    id: 0
	 *	    guid: 14081435818166446876
	 *	    path: '/dev/dsk/c2t0d0s0'
	 *	    phys_path: '/xpvd/xdf@51712:a'
	 */

	/* Get the 'vdev_tree' value which is itself an nvlist */
	assert(nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
	    &vdt) == 0);

	/* Report the current values */
	assert(nvlist_lookup_string(vdt, ZPOOL_CONFIG_PATH, &s) == 0);
	printf("                 Path: '%s'\n", s);
	assert(nvlist_lookup_string(vdt, ZPOOL_CONFIG_PHYS_PATH, &s) == 0);
	printf("        Physical path: '%s'\n", s);

	/* Update the values */
	assert(nvlist_remove_all(vdt, ZPOOL_CONFIG_PHYS_PATH) == 0);
	assert(nvlist_remove_all(vdt, ZPOOL_CONFIG_PATH) == 0);

	assert(nvlist_add_string(vdt, ZPOOL_CONFIG_PHYS_PATH, XEN_PHYS) == 0);
	assert(nvlist_add_string(vdt, ZPOOL_CONFIG_PATH, XEN_PATH) == 0);

	/* Output the new pool configuration */
	printf("Updated paths\n");
	if (verbose)
	{
		printf("\n");
		dump_nvlist(config, 16);
		printf("\n");
	}

	/* Pack the nvlist... */
	buf = phys->vp_nvlist;
	buflen = VDEV_PHYS_SIZE;
	assert(nvlist_pack(config, &buf, &buflen, NV_ENCODE_XDR, 0) == 0);
	printf("Packed nvlist\n");

	/* ...fix the checksum */
	zio_checksum_SHA256(phys, VDEV_PHYS_SIZE, NULL,
	    &phys->vp_zbt.zec_cksum);
	printf("Computed new checksum\n");

	/* ...and write the updated vdev_phys_t to the disk */

	for (label = 0; label < VDEV_LABELS; label++)
	{
		assert(pwrite64(fd, phys, VDEV_PHYS_SIZE,
		    label_offset(size, label) +
		    offsetof(vdev_label_t, vl_vdev_phys)) == VDEV_PHYS_SIZE);
		printf("Wrote label %d to disk\n", label);
		// Currently, writing labels 2 & 3 breaks the pool
		// but we only need to write the first label anyway.
		break;
	}

	/*********************************************************************/

	assert(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_TXG, &txg) == 0);
	assert(nvlist_lookup_uint64(vdt, ZPOOL_CONFIG_ASHIFT, &ashift) == 0);

	printf("Pool txg = %lu (ashift=%d)\n", txg, ashift);

	/* Find the uberblock */

	vdev_t vd;
	vdev_t *vdp = &vd;
	uberblock_t *ub;

	vdp->v_ashift = ashift;
	vdp->v_top = vdp;

	for (i = 0; i < VDEV_UBERBLOCK_COUNT(vdp); i++)
	{
		uint64_t uoff = VDEV_UBERBLOCK_OFFSET(vdp, i);
		time_t timestamp;

		ub = (void *)((char *)&vl + uoff);
		timestamp = ub->ub_timestamp;

		if (ub->ub_magic != UBERBLOCK_MAGIC || !ub->ub_txg)
			continue;

		if (ub->ub_txg == txg)
			break;
	}

	printf("Found UB %d\n", ub->ub_txg);

	/* Find and load the MOS */

	blkptr_t *bp = &ub->ub_rootbp;
        objset_phys_t mos;

	bpread(fd, bp, &mos, sizeof(mos));

	assert(mos.os_type == DMU_OST_META);

	/* Retrieve the directory */

	dnode_phys_t dir;

	dn_read(fd, &mos.os_meta_dnode,
	    DMU_POOL_DIRECTORY_OBJECT * sizeof(dnode_phys_t),
	    &dir, sizeof(dir));

	//hexdump(&dir, sizeof(dir));
}

int
main(int argc, char **argv)
{
	struct stat64 st;
	vdev_label_t vl;
	nvlist_t *config;
	uint64_t size;
	int fd;

	if (argc >= 2 && !strcmp(argv[1], "-v"))
		verbose++, argc--, argv++;

	if (argc != 2)
	{
		fprintf(stderr, "Syntax: %s [-v] <path to vdev>\n", argv[0]);
		return -1;
	}

	if ((fd = open(argv[1], O_RDWR)) == -1)
	{
		perror("open");
		return 1;
	}

	if (fstat64(fd, &st) == -1)
	{
		perror("fstat");
		return 0;
	}
	size = P2ALIGN_TYPED(st.st_size, sizeof(vdev_label_t), uint64_t);

	update_vdev_labels(fd, size);

	fsync(fd);
	close(fd);

	return 0;
}

// vim:fdm=marker
