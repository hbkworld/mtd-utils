/*
 * Copyright (C) 2008 Nokia Corporation.
 * Copyright (C) 2008 University of Szeged, Hungary
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Authors: Adrian Hunter
 *          Artem Bityutskiy
 *          Zoltan Sogor
 */

#define _XOPEN_SOURCE 500 /* For realpath() */

#include "mkfs.ubifs.h"
#include <crc32.h>
#include "common.h"
#include <sys/types.h>
#ifndef WITHOUT_XATTR
#include <sys/xattr.h>
#endif

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
#include <selinux/label.h>
#endif

#include "crypto.h"

/* Size (prime number) of hash table for link counting */
#define HASH_TABLE_SIZE 10099

/* The node buffer must allow for worst case compression */
#define NODE_BUFFER_SIZE (UBIFS_DATA_NODE_SZ + \
			  UBIFS_BLOCK_SIZE * WORST_COMPR_FACTOR)

/* Default time granularity in nanoseconds */
#define DEFAULT_TIME_GRAN 1000000000


#ifdef WITH_SELINUX
#define XATTR_NAME_SELINUX "security.selinux"
static struct selabel_handle *sehnd;
static char *secontext;
#endif

/**
 * struct idx_entry - index entry.
 * @next: next index entry (NULL at end of list)
 * @prev: previous index entry (NULL at beginning of list)
 * @key: key
 * @name: directory entry name used for sorting colliding keys by name
 * @lnum: LEB number
 * @offs: offset
 * @len: length
 *
 * The index is recorded as a linked list which is sorted and used to create
 * the bottom level of the on-flash index tree. The remaining levels of the
 * index tree are each built from the level below.
 */
struct idx_entry {
	struct idx_entry *next;
	struct idx_entry *prev;
	union ubifs_key key;
	char *name;
	int name_len;
	int lnum;
	int offs;
	int len;
};

/**
 * struct inum_mapping - inode number mapping for link counting.
 * @next: next inum_mapping (NULL at end of list)
 * @prev: previous inum_mapping (NULL at beginning of list)
 * @dev: source device on which the source inode number resides
 * @inum: source inode number of the file
 * @use_inum: target inode number of the file
 * @use_nlink: number of links
 * @path_name: a path name of the file
 * @st: struct stat object containing inode attributes which have to be used
 *      when the inode is being created (actually only UID, GID, access
 *      mode, major and minor device numbers)
 *
 * If a file has more than one hard link, then the number of hard links that
 * exist in the source directory hierarchy must be counted to exclude the
 * possibility that the file is linked from outside the source directory
 * hierarchy.
 *
 * The inum_mappings are stored in a hash_table of linked lists.
 */
struct inum_mapping {
	struct inum_mapping *next;
	struct inum_mapping *prev;
	dev_t dev;
	ino_t inum;
	ino_t use_inum;
	unsigned int use_nlink;
	char *path_name;
	struct stat st;
};

#ifndef FS_KEY_DESCRIPTOR_SIZE
#define FS_KEY_DESCRIPTOR_SIZE  8
#endif
#define FS_ENCRYPTION_CONTEXT_FORMAT_V1 1
#define FS_KEY_DERIVATION_NONCE_SIZE	16

#ifndef FS_ENCRYPTION_MODE_AES_128_CBC
#define FS_ENCRYPTION_MODE_AES_128_CBC 5
#endif

#ifndef FS_ENCRYPTION_MODE_AES_128_CTS
#define FS_ENCRYPTION_MODE_AES_128_CTS 6
#endif

#ifndef FS_POLICY_FLAGS_VALID
#define FS_POLICY_FLAGS_PAD_4		0x00
#define FS_POLICY_FLAGS_PAD_8		0x01
#define FS_POLICY_FLAGS_PAD_16		0x02
#define FS_POLICY_FLAGS_PAD_32		0x03
#define FS_POLICY_FLAGS_PAD_MASK	0x03
#define FS_POLICY_FLAGS_VALID		0x03
#endif

#define FS_CRYPTO_BLOCK_SIZE	16

/**
 * Encryption context for inode
 *
 * Protector format:
 *  1 byte: Protector format (1 = this version)
 *  1 byte: File contents encryption mode
 *  1 byte: File names encryption mode
 *  1 byte: Flags
 *  8 bytes: Master Key descriptor
 *  16 bytes: Encryption Key derivation nonce
 */
struct fscrypt_context {
	__u8 format;
	__u8 contents_encryption_mode;
	__u8 filenames_encryption_mode;
	__u8 flags;
	__u8 master_key_descriptor[FS_KEY_DESCRIPTOR_SIZE];
	__u8 nonce[FS_KEY_DERIVATION_NONCE_SIZE];
} __attribute__((packed));

/**
 * For encrypted symlinks, the ciphertext length is stored at the beginning
 * of the string in little-endian format.
 */
struct fscrypt_symlink_data {
	__le16 len;
	char encrypted_path[1];
} __attribute__((packed));


#ifndef FS_MAX_KEY_SIZE
#define FS_MAX_KEY_SIZE	64
#endif
static __u8 fscrypt_masterkey[FS_MAX_KEY_SIZE];

/*
 * Because we copy functions from the kernel, we use a subset of the UBIFS
 * file-system description object struct ubifs_info.
 */
struct ubifs_info info_;
static struct ubifs_info *c = &info_;
static libubi_t ubi;

/* Debug levels are: 0 (none), 1 (statistics), 2 (files) ,3 (more details) */
int debug_level;
int verbose;
int yes;

static char *root;
static int root_len;
static struct fscrypt_context *root_fctx;
static struct stat root_st;
static char *output;
static int out_fd;
static int out_ubi;
static int squash_owner;
static int do_create_inum_attr;
static char *context;
static int context_len;
static struct stat context_st;

/* The 'head' (position) which nodes are written */
static int head_lnum;
static int head_offs;
static int head_flags;

/* The index list */
static struct idx_entry *idx_list_first;
static struct idx_entry *idx_list_last;
static size_t idx_cnt;

/* Global buffers */
static void *leb_buf;
static void *node_buf;
static void *block_buf;

/* Hash table for inode link counting */
static struct inum_mapping **hash_table;

/* Inode creation sequence number */
static unsigned long long creat_sqnum;

static const char *optstring = "d:r:m:o:D:yh?vVe:c:g:f:Fp:k:x:X:j:R:l:j:UQqa";

static const struct option longopts[] = {
	{"root",               1, NULL, 'r'},
	{"min-io-size",        1, NULL, 'm'},
	{"leb-size",           1, NULL, 'e'},
	{"max-leb-cnt",        1, NULL, 'c'},
	{"output",             1, NULL, 'o'},
	{"devtable",           1, NULL, 'D'},
	{"yes",                0, NULL, 'y'},
	{"help",               0, NULL, 'h'},
	{"verbose",            0, NULL, 'v'},
	{"version",            0, NULL, 'V'},
	{"debug-level",        1, NULL, 'g'},
	{"jrn-size",           1, NULL, 'j'},
	{"reserved",           1, NULL, 'R'},
	{"compr",              1, NULL, 'x'},
	{"favor-percent",      1, NULL, 'X'},
	{"fanout",             1, NULL, 'f'},
	{"space-fixup",        0, NULL, 'F'},
	{"keyhash",            1, NULL, 'k'},
	{"log-lebs",           1, NULL, 'l'},
	{"orph-lebs",          1, NULL, 'p'},
	{"squash-uids" ,       0, NULL, 'U'},
	{"set-inode-attr",     0, NULL, 'a'},
	{"selinux",            1, NULL, 's'},
	{NULL, 0, NULL, 0}
};

static const char *helptext =
"Usage: mkfs.ubifs [OPTIONS] target\n"
"Make a UBIFS file system image from an existing directory tree\n\n"
"Examples:\n"
"Build file system from directory /opt/img, writting the result in the ubifs.img file\n"
"\tmkfs.ubifs -m 512 -e 128KiB -c 100 -r /opt/img ubifs.img\n"
"The same, but writting directly to an UBI volume\n"
"\tmkfs.ubifs -r /opt/img /dev/ubi0_0\n"
"Creating an empty UBIFS filesystem on an UBI volume\n"
"\tmkfs.ubifs /dev/ubi0_0\n\n"
"Options:\n"
"-r, -d, --root=DIR       build file system from directory DIR\n"
"-m, --min-io-size=SIZE   minimum I/O unit size\n"
"-e, --leb-size=SIZE      logical erase block size\n"
"-c, --max-leb-cnt=COUNT  maximum logical erase block count\n"
"-o, --output=FILE        output to FILE\n"
"-j, --jrn-size=SIZE      journal size\n"
"-R, --reserved=SIZE      how much space should be reserved for the super-user\n"
"-x, --compr=TYPE         compression type - \"lzo\", \"favor_lzo\", \"zlib\" or\n"
"                         \"none\" (default: \"lzo\")\n"
"-X, --favor-percent      may only be used with favor LZO compression and defines\n"
"                         how many percent better zlib should compress to make\n"
"                         mkfs.ubifs use zlib instead of LZO (default 20%)\n"
"-f, --fanout=NUM         fanout NUM (default: 8)\n"
"-F, --space-fixup        file-system free space has to be fixed up on first mount\n"
"                         (requires kernel version 3.0 or greater)\n"
"-k, --keyhash=TYPE       key hash type - \"r5\" or \"test\" (default: \"r5\")\n"
"-p, --orph-lebs=COUNT    count of erase blocks for orphans (default: 1)\n"
"-D, --devtable=FILE      use device table FILE\n"
"-U, --squash-uids        squash owners making all files owned by root\n"
"-l, --log-lebs=COUNT     count of erase blocks for the log (used only for\n"
"                         debugging)\n"
"-y, --yes                assume the answer is \"yes\" for all questions\n"
"-v, --verbose            verbose operation\n"
"-V, --version            display version information\n"
"-g, --debug=LEVEL        display debug information (0 - none, 1 - statistics,\n"
"                         2 - files, 3 - more details)\n"
"-a, --set-inum-attr      create user.image-inode-number extended attribute on files\n"
"                         added to the image. The attribute will contain the inode\n"
"                         number the file has in the generated image.\n"
"-s, --selinux=FILE       Selinux context file\n"
"-h, --help               display this help text\n\n"
"Note, SIZE is specified in bytes, but it may also be specified in Kilobytes,\n"
"Megabytes, and Gigabytes if a KiB, MiB, or GiB suffix is used.\n\n"
"If you specify \"lzo\" or \"zlib\" compressors, mkfs.ubifs will use this compressor\n"
"for all data. The \"none\" disables any data compression. The \"favor_lzo\" is not\n"
"really a separate compressor. It is just a method of combining \"lzo\" and \"zlib\"\n"
"compressors. Namely, mkfs.ubifs tries to compress data with both \"lzo\" and \"zlib\"\n"
"compressors, then it compares which compressor is better. If \"zlib\" compresses 20\n"
"or more percent better than \"lzo\", mkfs.ubifs chooses \"lzo\", otherwise it chooses\n"
"\"zlib\". The \"--favor-percent\" may specify arbitrary threshold instead of the\n"
"default 20%.\n\n"
"The -F parameter is used to set the \"fix up free space\" flag in the superblock,\n"
"which forces UBIFS to \"fixup\" all the free space which it is going to use. This\n"
"option is useful to work-around the problem of double free space programming: if the\n"
"flasher program which flashes the UBI image is unable to skip NAND pages containing\n"
"only 0xFF bytes, the effect is that some NAND pages are written to twice - first time\n"
"when flashing the image and the second time when UBIFS is mounted and writes useful\n"
"data there. A proper UBI-aware flasher should skip such NAND pages, though. Note, this\n"
"flag may make the first mount very slow, because the \"free space fixup\" procedure\n"
"takes time. This feature is supported by the Linux kernel starting from version 3.0.\n";

/**
 * make_path - make a path name from a directory and a name.
 * @dir: directory path name
 * @name: name
 */
static char *make_path(const char *dir, const char *name)
{
	char *s;

	xasprintf(&s, "%s%s%s",
		  dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);

	return s;
}

/**
 * is_contained - determine if a file is beneath a directory.
 * @file: file path name
 * @dir: directory path name
 *
 * This function returns %1 if @file is accessible from the @dir directory and
 * %0 otherwise. In case of error, returns %-1.
 */
static int is_contained(const char *file, const char *dir)
{
	char *real_file = NULL;
	char *real_dir = NULL;
	char *file_base, *copy;
	int ret = -1;

	/* Make a copy of the file path because 'dirname()' can modify it */
	copy = strdup(file);
	if (!copy)
		return -1;
	file_base = dirname(copy);

	/* Turn the paths into the canonical form */
	real_file = xmalloc(PATH_MAX);
	real_dir = xmalloc(PATH_MAX);

	if (!realpath(file_base, real_file)) {
		perror("Could not canonicalize file path");
		goto out_free;
	}
	if (!realpath(dir, real_dir)) {
		perror("Could not canonicalize directory");
		goto out_free;
	}

	ret = !!strstr(real_file, real_dir);

out_free:
	free(copy);
	free(real_file);
	free(real_dir);
	return ret;
}

/**
 * calc_min_log_lebs - calculate the minimum number of log LEBs needed.
 * @max_bud_bytes: journal size (buds only)
 */
static int calc_min_log_lebs(unsigned long long max_bud_bytes)
{
	int buds, log_lebs;
	unsigned long long log_size;

	buds = (max_bud_bytes + c->leb_size - 1) / c->leb_size;
	log_size = ALIGN(UBIFS_REF_NODE_SZ, c->min_io_size);
	log_size *= buds;
	log_size += ALIGN(UBIFS_CS_NODE_SZ +
			  UBIFS_REF_NODE_SZ * (c->jhead_cnt + 2),
			  c->min_io_size);
	log_lebs = (log_size + c->leb_size - 1) / c->leb_size;
	log_lebs += 1;
	return log_lebs;
}

/**
 * add_space_overhead - add UBIFS overhead.
 * @size: flash space which should be visible to the user
 *
 * UBIFS has overhead, and if we need to reserve @size bytes for the user data,
 * we have to reserve more flash space, to compensate the overhead. This
 * function calculates and returns the amount of physical flash space which
 * should be reserved to provide @size bytes for the user.
 */
static long long add_space_overhead(long long size)
{
        int divisor, factor, f, max_idx_node_sz;

        /*
	 * Do the opposite to what the 'ubifs_reported_space()' kernel UBIFS
	 * function does.
         */
	max_idx_node_sz =  ubifs_idx_node_sz(c, c->fanout);
        f = c->fanout > 3 ? c->fanout >> 1 : 2;
        divisor = UBIFS_BLOCK_SIZE;
        factor = UBIFS_MAX_DATA_NODE_SZ;
        factor += (max_idx_node_sz * 3) / (f - 1);
        size *= factor;
        return size / divisor;
}

static int validate_options(void)
{
	int tmp;

	if (!output)
		return err_msg("no output file or UBI volume specified");
	if (root) {
		tmp = is_contained(output, root);
		if (tmp < 0)
			return err_msg("failed to perform output file root check");
		else if (tmp)
			return err_msg("output file cannot be in the UBIFS root "
			               "directory");
	}
	if (!is_power_of_2(c->min_io_size))
		return err_msg("min. I/O unit size should be power of 2");
	if (c->leb_size < c->min_io_size)
		return err_msg("min. I/O unit cannot be larger than LEB size");
	if (c->leb_size < UBIFS_MIN_LEB_SZ)
		return err_msg("too small LEB size %d, minimum is %d",
			       c->leb_size, UBIFS_MIN_LEB_SZ);
	if (c->leb_size % c->min_io_size)
		return err_msg("LEB should be multiple of min. I/O units");
	if (c->leb_size % 8)
		return err_msg("LEB size has to be multiple of 8");
	if (c->leb_size > UBIFS_MAX_LEB_SZ)
		return err_msg("too large LEB size %d, maximum is %d",
				c->leb_size, UBIFS_MAX_LEB_SZ);
	if (c->max_leb_cnt < UBIFS_MIN_LEB_CNT)
		return err_msg("too low max. count of LEBs, minimum is %d",
			       UBIFS_MIN_LEB_CNT);
	if (c->fanout < UBIFS_MIN_FANOUT)
		return err_msg("too low fanout, minimum is %d",
			       UBIFS_MIN_FANOUT);
	tmp = c->leb_size - UBIFS_IDX_NODE_SZ;
	tmp /= UBIFS_BRANCH_SZ + UBIFS_MAX_KEY_LEN;
	if (c->fanout > tmp)
		return err_msg("too high fanout, maximum is %d", tmp);
	if (c->log_lebs < UBIFS_MIN_LOG_LEBS)
		return err_msg("too few log LEBs, minimum is %d",
			       UBIFS_MIN_LOG_LEBS);
	if (c->log_lebs >= c->max_leb_cnt - UBIFS_MIN_LEB_CNT)
		return err_msg("too many log LEBs, maximum is %d",
			       c->max_leb_cnt - UBIFS_MIN_LEB_CNT);
	if (c->orph_lebs < UBIFS_MIN_ORPH_LEBS)
		return err_msg("too few orphan LEBs, minimum is %d",
			       UBIFS_MIN_ORPH_LEBS);
	if (c->orph_lebs >= c->max_leb_cnt - UBIFS_MIN_LEB_CNT)
		return err_msg("too many orphan LEBs, maximum is %d",
			       c->max_leb_cnt - UBIFS_MIN_LEB_CNT);
	tmp = UBIFS_SB_LEBS + UBIFS_MST_LEBS + c->log_lebs + c->lpt_lebs;
	tmp += c->orph_lebs + 4;
	if (tmp > c->max_leb_cnt)
		return err_msg("too low max. count of LEBs, expected at "
			       "least %d", tmp);
	tmp = calc_min_log_lebs(c->max_bud_bytes);
	if (c->log_lebs < calc_min_log_lebs(c->max_bud_bytes))
		return err_msg("too few log LEBs, expected at least %d", tmp);
	if (c->rp_size >= ((long long)c->leb_size * c->max_leb_cnt) / 2)
		return err_msg("too much reserved space %lld", c->rp_size);
	return 0;
}

/**
 * get_multiplier - convert size specifier to an integer multiplier.
 * @str: the size specifier string
 *
 * This function parses the @str size specifier, which may be one of
 * 'KiB', 'MiB', or 'GiB' into an integer multiplier. Returns positive
 * size multiplier in case of success and %-1 in case of failure.
 */
static int get_multiplier(const char *str)
{
	if (!str)
		return 1;

	/* Remove spaces before the specifier */
	while (*str == ' ' || *str == '\t')
		str += 1;

	if (!strcmp(str, "KiB"))
		return 1024;
	if (!strcmp(str, "MiB"))
		return 1024 * 1024;
	if (!strcmp(str, "GiB"))
		return 1024 * 1024 * 1024;

	return -1;
}

/**
 * get_bytes - convert a string containing amount of bytes into an
 *             integer.
 * @str: string to convert
 *
 * This function parses @str which may have one of 'KiB', 'MiB', or 'GiB' size
 * specifiers. Returns positive amount of bytes in case of success and %-1 in
 * case of failure.
 */
static long long get_bytes(const char *str)
{
	char *endp;
	long long bytes = strtoull(str, &endp, 0);

	if (endp == str || bytes < 0)
		return err_msg("incorrect amount of bytes: \"%s\"", str);

	if (*endp != '\0') {
		int mult = get_multiplier(endp);

		if (mult == -1)
			return err_msg("bad size specifier: \"%s\" - "
				       "should be 'KiB', 'MiB' or 'GiB'", endp);
		bytes *= mult;
	}

	return bytes;
}

static unsigned char *calc_fscrypt_subkey(struct fscrypt_context *fctx)
{
	int ret;
	unsigned char *new_key = xmalloc(FS_MAX_KEY_SIZE);

	ret = derive_key_aes(fctx->nonce, fscrypt_masterkey, FS_MAX_KEY_SIZE, new_key);
	if (ret < 0) {
		err_msg("derive_key_aes failed: %i\n", ret);

		free(new_key);
		new_key = NULL;
	}

	return new_key;
}

static struct fscrypt_context *inherit_fscrypt_context(struct fscrypt_context *fctx)
{
	struct fscrypt_context *new_fctx = NULL;

	if (fctx) {
		new_fctx = xmalloc(sizeof(*new_fctx));
		new_fctx->format = fctx->format;
		new_fctx->contents_encryption_mode = fctx->contents_encryption_mode;
		new_fctx->filenames_encryption_mode = fctx->filenames_encryption_mode;
		new_fctx->flags = fctx->flags;
		memcpy(new_fctx->master_key_descriptor, fctx->master_key_descriptor,
		       FS_KEY_DESCRIPTOR_SIZE);
		RAND_bytes((void *)&new_fctx->nonce, FS_KEY_DERIVATION_NONCE_SIZE);
	}

	return new_fctx;
}

static void free_fscrypt_context(struct fscrypt_context *fctx)
{
	free(fctx);
}

static void print_fscrypt_master_key_descriptor(struct fscrypt_context *fctx)
{
	int i;

	normsg_cont("fscrypt master key descriptor: ");
	for (i = 0; i < FS_KEY_DESCRIPTOR_SIZE; i++) {
		normsg_cont("%02x", fctx->master_key_descriptor[i]);
	}
	normsg("");
}

static struct fscrypt_context *init_fscrypt_context(void)
{
	struct fscrypt_context *new_fctx = xmalloc(sizeof(*new_fctx));

	new_fctx->format = FS_ENCRYPTION_CONTEXT_FORMAT_V1;
	new_fctx->contents_encryption_mode = FS_ENCRYPTION_MODE_AES_128_CBC;
	new_fctx->filenames_encryption_mode = FS_ENCRYPTION_MODE_AES_128_CTS;
	new_fctx->flags = FS_POLICY_FLAGS_PAD_4;
	RAND_bytes((void *)&new_fctx->nonce, FS_KEY_DERIVATION_NONCE_SIZE);

	return new_fctx;
}

unsigned int fscrypt_fname_encrypted_size(struct fscrypt_context *fctx, unsigned int ilen)
{
	int padding;

	padding = 4 << (fctx->flags & FS_POLICY_FLAGS_PAD_MASK);
	ilen = max_t(unsigned int, ilen, FS_CRYPTO_BLOCK_SIZE);
	return round_up(ilen, padding);
}


/**
 * open_ubi - open the UBI volume.
 * @node: name of the UBI volume character device to fetch information about
 *
 * Returns %0 in case of success and %-1 in case of failure
 */
static int open_ubi(const char *node)
{
	struct stat st;

	if (stat(node, &st) || !S_ISCHR(st.st_mode))
		return -1;

	ubi = libubi_open();
	if (!ubi)
		return -1;
	if (ubi_get_vol_info(ubi, node, &c->vi))
		return -1;
	if (ubi_get_dev_info1(ubi, c->vi.dev_num, &c->di))
		return -1;
	return 0;
}

static int get_options(int argc, char**argv)
{
	int opt, i;
	const char *tbl_file = NULL;
	struct stat st;
	char *endp;

	c->fanout = 8;
	c->orph_lebs = 1;
	c->key_hash = key_r5_hash;
	c->key_len = UBIFS_SK_LEN;
#ifdef WITHOUT_LZO
	c->default_compr = UBIFS_COMPR_ZLIB;
#else
	c->default_compr = UBIFS_COMPR_LZO;
#endif
	c->favor_percent = 20;
	c->lsave_cnt = 256;
	c->leb_size = -1;
	c->min_io_size = -1;
	c->max_leb_cnt = -1;
	c->max_bud_bytes = -1;
	c->log_lebs = -1;

	while (1) {
		opt = getopt_long(argc, argv, optstring, longopts, &i);
		if (opt == -1)
			break;
		switch (opt) {
		case 'r':
		case 'd':
			root_len = strlen(optarg);
			root = xmalloc(root_len + 2);

			/*
			 * The further code expects '/' at the end of the root
			 * UBIFS directory on the host.
			 */
			memcpy(root, optarg, root_len);
			if (root[root_len - 1] != '/')
				root[root_len++] = '/';
			root[root_len] = 0;

			/* Make sure the root directory exists */
			if (stat(root, &st))
				return sys_err_msg("bad root directory '%s'",
						   root);
			break;
		case 'm':
			c->min_io_size = get_bytes(optarg);
			if (c->min_io_size <= 0)
				return err_msg("bad min. I/O size");
			break;
		case 'e':
			c->leb_size = get_bytes(optarg);
			if (c->leb_size <= 0)
				return err_msg("bad LEB size");
			break;
		case 'c':
			c->max_leb_cnt = get_bytes(optarg);
			if (c->max_leb_cnt <= 0)
				return err_msg("bad maximum LEB count");
			break;
		case 'o':
			output = xstrdup(optarg);
			break;
		case 'D':
			tbl_file = optarg;
			if (stat(tbl_file, &st) < 0)
				return sys_err_msg("bad device table file '%s'",
						   tbl_file);
			break;
		case 'y':
			yes = 1;
			break;
		case 'h':
			printf("%s", helptext);
			exit(EXIT_SUCCESS);
		case '?':
			printf("%s", helptext);
			exit(-1);
		case 'v':
			verbose = 1;
			break;
		case 'V':
			common_print_version();
			exit(EXIT_SUCCESS);
		case 'g':
			debug_level = strtol(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg ||
			    debug_level < 0 || debug_level > 3)
				return err_msg("bad debugging level '%s'",
					       optarg);
			break;
		case 'f':
			c->fanout = strtol(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg || c->fanout <= 0)
				return err_msg("bad fanout %s", optarg);
			break;
		case 'F':
			c->space_fixup = 1;
			break;
		case 'l':
			c->log_lebs = strtol(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg || c->log_lebs <= 0)
				return err_msg("bad count of log LEBs '%s'",
					       optarg);
			break;
		case 'p':
			c->orph_lebs = strtol(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg ||
			    c->orph_lebs <= 0)
				return err_msg("bad orphan LEB count '%s'",
					       optarg);
			break;
		case 'k':
			if (strcmp(optarg, "r5") == 0) {
				c->key_hash = key_r5_hash;
				c->key_hash_type = UBIFS_KEY_HASH_R5;
			} else if (strcmp(optarg, "test") == 0) {
				c->key_hash = key_test_hash;
				c->key_hash_type = UBIFS_KEY_HASH_TEST;
			} else
				return err_msg("bad key hash");
			break;
		case 'x':
			if (strcmp(optarg, "none") == 0)
				c->default_compr = UBIFS_COMPR_NONE;
			else if (strcmp(optarg, "zlib") == 0)
				c->default_compr = UBIFS_COMPR_ZLIB;
#ifndef WITHOUT_LZO
			else if (strcmp(optarg, "favor_lzo") == 0)
				c->favor_lzo = 1;
			else if (strcmp(optarg, "lzo") != 0)
#else
			else
#endif
				return err_msg("bad compressor name");
			break;
		case 'X':
#ifdef WITHOUT_LZO
			return err_msg("built without LZO support");
#else
			c->favor_percent = strtol(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg ||
			    c->favor_percent <= 0 || c->favor_percent >= 100)
				return err_msg("bad favor LZO percent '%s'",
					       optarg);
#endif
			break;
		case 'j':
			c->max_bud_bytes = get_bytes(optarg);
			if (c->max_bud_bytes <= 0)
				return err_msg("bad maximum amount of buds");
			break;
		case 'R':
			c->rp_size = get_bytes(optarg);
			if (c->rp_size < 0)
				return err_msg("bad reserved bytes count");
			break;
		case 'U':
			squash_owner = 1;
			break;
		case 'a':
			do_create_inum_attr = 1;
			break;
		case 's':
			context_len = strlen(optarg);
			context = (char *) xmalloc(context_len + 1);
			if (!context)
				return err_msg("xmalloc failed\n");
			memcpy(context, optarg, context_len);

			/* Make sure root directory exists */
			if (stat(context, &context_st))
				return sys_err_msg("bad file context %s\n",
								   context);
			break;
		}
	}

	if (optind != argc && !output)
		output = xstrdup(argv[optind]);

	if (!output)
		return err_msg("not output device or file specified");

	out_ubi = !open_ubi(output);

	if (out_ubi) {
		c->min_io_size = c->di.min_io_size;
		c->leb_size = c->vi.leb_size;
		if (c->max_leb_cnt == -1)
			c->max_leb_cnt = c->vi.rsvd_lebs;
	}

	if (c->min_io_size == -1)
		return err_msg("min. I/O unit was not specified "
			       "(use -h for help)");

	if (c->leb_size == -1)
		return err_msg("LEB size was not specified (use -h for help)");

	if (c->max_leb_cnt == -1)
		return err_msg("Maximum count of LEBs was not specified "
			       "(use -h for help)");

	if (c->max_bud_bytes == -1) {
		int lebs;

		lebs = c->max_leb_cnt - UBIFS_SB_LEBS - UBIFS_MST_LEBS;
		lebs -= c->orph_lebs;
		if (c->log_lebs != -1)
			lebs -= c->log_lebs;
		else
			lebs -= UBIFS_MIN_LOG_LEBS;
		/*
		 * We do not know lprops geometry so far, so assume minimum
		 * count of lprops LEBs.
		 */
		lebs -= UBIFS_MIN_LPT_LEBS;
		/* Make the journal about 12.5% of main area lebs */
		c->max_bud_bytes = (lebs / 8) * (long long)c->leb_size;
		/* Make the max journal size 8MiB */
		if (c->max_bud_bytes > 8 * 1024 * 1024)
			c->max_bud_bytes = 8 * 1024 * 1024;
		if (c->max_bud_bytes < 4 * c->leb_size)
			c->max_bud_bytes = 4 * c->leb_size;
	}

	if (c->log_lebs == -1) {
		c->log_lebs = calc_min_log_lebs(c->max_bud_bytes);
		c->log_lebs += 2;
	}

	if (c->min_io_size < 8)
		c->min_io_size = 8;
	c->rp_size = add_space_overhead(c->rp_size);

	if (verbose) {
		printf("mkfs.ubifs\n");
		printf("\troot:         %s\n", root);
		printf("\tmin_io_size:  %d\n", c->min_io_size);
		printf("\tleb_size:     %d\n", c->leb_size);
		printf("\tmax_leb_cnt:  %d\n", c->max_leb_cnt);
		printf("\toutput:       %s\n", output);
		printf("\tjrn_size:     %llu\n", c->max_bud_bytes);
		printf("\treserved:     %llu\n", c->rp_size);
		switch (c->default_compr) {
		case UBIFS_COMPR_LZO:
			printf("\tcompr:        lzo\n");
			break;
		case UBIFS_COMPR_ZLIB:
			printf("\tcompr:        zlib\n");
			break;
		case UBIFS_COMPR_NONE:
			printf("\tcompr:        none\n");
			break;
		}
		printf("\tkeyhash:      %s\n", (c->key_hash == key_r5_hash) ?
						"r5" : "test");
		printf("\tfanout:       %d\n", c->fanout);
		printf("\torph_lebs:    %d\n", c->orph_lebs);
		printf("\tspace_fixup:  %d\n", c->space_fixup);
		printf("\tselinux file: %s\n", context);
	}

	if (validate_options())
		return -1;

	if (tbl_file && parse_devtable(tbl_file))
		return err_msg("cannot parse device table file '%s'", tbl_file);

	return 0;
}

/**
 * prepare_node - fill in the common header.
 * @node: node
 * @len: node length
 */
static void prepare_node(void *node, int len)
{
	uint32_t crc;
	struct ubifs_ch *ch = node;

	ch->magic = cpu_to_le32(UBIFS_NODE_MAGIC);
	ch->len = cpu_to_le32(len);
	ch->group_type = UBIFS_NO_NODE_GROUP;
	ch->sqnum = cpu_to_le64(++c->max_sqnum);
	ch->padding[0] = ch->padding[1] = 0;
	crc = mtd_crc32(UBIFS_CRC32_INIT, node + 8, len - 8);
	ch->crc = cpu_to_le32(crc);
}

/**
 * write_leb - copy the image of a LEB to the output target.
 * @lnum: LEB number
 * @len: length of data in the buffer
 * @buf: buffer (must be at least c->leb_size bytes)
 */
int write_leb(int lnum, int len, void *buf)
{
	off_t pos = (off_t)lnum * c->leb_size;

	dbg_msg(3, "LEB %d len %d", lnum, len);
	memset(buf + len, 0xff, c->leb_size - len);
	if (out_ubi)
		if (ubi_leb_change_start(ubi, out_fd, lnum, c->leb_size))
			return sys_err_msg("ubi_leb_change_start failed");

	if (lseek(out_fd, pos, SEEK_SET) != pos)
		return sys_err_msg("lseek failed seeking %lld", (long long)pos);

	if (write(out_fd, buf, c->leb_size) != c->leb_size)
		return sys_err_msg("write failed writing %d bytes at pos %lld",
				   c->leb_size, (long long)pos);

	return 0;
}

/**
 * write_empty_leb - copy the image of an empty LEB to the output target.
 * @lnum: LEB number
 */
static int write_empty_leb(int lnum)
{
	return write_leb(lnum, 0, leb_buf);
}

/**
 * do_pad - pad a buffer to the minimum I/O size.
 * @buf: buffer
 * @len: buffer length
 */
static int do_pad(void *buf, int len)
{
	int pad_len, alen = ALIGN(len, 8), wlen = ALIGN(alen, c->min_io_size);
	uint32_t crc;

	memset(buf + len, 0xff, alen - len);
	pad_len = wlen - alen;
	dbg_msg(3, "len %d pad_len %d", len, pad_len);
	buf += alen;
	if (pad_len >= (int)UBIFS_PAD_NODE_SZ) {
		struct ubifs_ch *ch = buf;
		struct ubifs_pad_node *pad_node = buf;

		ch->magic      = cpu_to_le32(UBIFS_NODE_MAGIC);
		ch->node_type  = UBIFS_PAD_NODE;
		ch->group_type = UBIFS_NO_NODE_GROUP;
		ch->padding[0] = ch->padding[1] = 0;
		ch->sqnum      = cpu_to_le64(0);
		ch->len        = cpu_to_le32(UBIFS_PAD_NODE_SZ);

		pad_len -= UBIFS_PAD_NODE_SZ;
		pad_node->pad_len = cpu_to_le32(pad_len);

		crc = mtd_crc32(UBIFS_CRC32_INIT, buf + 8,
				  UBIFS_PAD_NODE_SZ - 8);
		ch->crc = cpu_to_le32(crc);

		memset(buf + UBIFS_PAD_NODE_SZ, 0, pad_len);
	} else if (pad_len > 0)
		memset(buf, UBIFS_PADDING_BYTE, pad_len);

	return wlen;
}

/**
 * write_node - write a node to a LEB.
 * @node: node
 * @len: node length
 * @lnum: LEB number
 */
static int write_node(void *node, int len, int lnum)
{
	prepare_node(node, len);

	memcpy(leb_buf, node, len);

	len = do_pad(leb_buf, len);

	return write_leb(lnum, len, leb_buf);
}

/**
 * calc_dark - calculate LEB dark space size.
 * @c: the UBIFS file-system description object
 * @spc: amount of free and dirty space in the LEB
 *
 * This function calculates amount of dark space in an LEB which has @spc bytes
 * of free and dirty space. Returns the calculations result.
 *
 * Dark space is the space which is not always usable - it depends on which
 * nodes are written in which order. E.g., if an LEB has only 512 free bytes,
 * it is dark space, because it cannot fit a large data node. So UBIFS cannot
 * count on this LEB and treat these 512 bytes as usable because it is not true
 * if, for example, only big chunks of uncompressible data will be written to
 * the FS.
 */
static int calc_dark(struct ubifs_info *c, int spc)
{
	if (spc < c->dark_wm)
		return spc;

	/*
	 * If we have slightly more space then the dark space watermark, we can
	 * anyway safely assume it we'll be able to write a node of the
	 * smallest size there.
	 */
	if (spc - c->dark_wm < (int)MIN_WRITE_SZ)
		return spc - MIN_WRITE_SZ;

	return c->dark_wm;
}

/**
 * set_lprops - set the LEB property values for a LEB.
 * @lnum: LEB number
 * @offs: end offset of data in the LEB
 * @flags: LEB property flags
 */
static void set_lprops(int lnum, int offs, int flags)
{
	int i = lnum - c->main_first, free, dirty;
	int a = max_t(int, c->min_io_size, 8);

	free = c->leb_size - ALIGN(offs, a);
	dirty = c->leb_size - free - ALIGN(offs, 8);
	dbg_msg(3, "LEB %d free %d dirty %d flags %d", lnum, free, dirty,
		flags);
	if (i < c->main_lebs) {
		c->lpt[i].free = free;
		c->lpt[i].dirty = dirty;
		c->lpt[i].flags = flags;
	}
	c->lst.total_free += free;
	c->lst.total_dirty += dirty;
	if (flags & LPROPS_INDEX)
		c->lst.idx_lebs += 1;
	else {
		int spc;

		spc = free + dirty;
		if (spc < c->dead_wm)
			c->lst.total_dead += spc;
		else
			c->lst.total_dark += calc_dark(c, spc);
		c->lst.total_used += c->leb_size - spc;
	}
}

/**
 * add_to_index - add a node key and position to the index.
 * @key: node key
 * @lnum: node LEB number
 * @offs: node offset
 * @len: node length
 */
static int add_to_index(union ubifs_key *key, char *name, int name_len, int lnum, int offs,
			int len)
{
	struct idx_entry *e;

	dbg_msg(3, "LEB %d offs %d len %d", lnum, offs, len);
	e = xmalloc(sizeof(struct idx_entry));
	e->next = NULL;
	e->prev = idx_list_last;
	e->key = *key;
	e->name = name;
	e->name_len = name_len;
	e->lnum = lnum;
	e->offs = offs;
	e->len = len;
	if (!idx_list_first)
		idx_list_first = e;
	if (idx_list_last)
		idx_list_last->next = e;
	idx_list_last = e;
	idx_cnt += 1;
	return 0;
}

/**
 * flush_nodes - write the current head and move the head to the next LEB.
 */
static int flush_nodes(void)
{
	int len, err;

	if (!head_offs)
		return 0;
	len = do_pad(leb_buf, head_offs);
	err = write_leb(head_lnum, len, leb_buf);
	if (err)
		return err;
	set_lprops(head_lnum, head_offs, head_flags);
	head_lnum += 1;
	head_offs = 0;
	return 0;
}

/**
 * reserve_space - reserve space for a node on the head.
 * @len: node length
 * @lnum: LEB number is returned here
 * @offs: offset is returned here
 */
static int reserve_space(int len, int *lnum, int *offs)
{
	int err;

	if (len > c->leb_size - head_offs) {
		err = flush_nodes();
		if (err)
			return err;
	}
	*lnum = head_lnum;
	*offs = head_offs;
	head_offs += ALIGN(len, 8);
	return 0;
}

/**
 * add_node - write a node to the head.
 * @key: node key
 * @node: node
 * @len: node length
 */
static int add_node(union ubifs_key *key, char *name, int name_len, void *node, int len)
{
	int err, lnum, offs;

	if (key_type(key) == UBIFS_DENT_KEY || key_type(key) == UBIFS_XENT_KEY) {
		if (!name)
			return err_msg("Directory entry or xattr without name!");
	} else {
		if (name)
			return err_msg("Name given for non dir/xattr node!");
	}

	prepare_node(node, len);

	err = reserve_space(len, &lnum, &offs);
	if (err)
		return err;

	memcpy(leb_buf + offs, node, len);
	memset(leb_buf + offs + len, 0xff, ALIGN(len, 8) - len);

	add_to_index(key, name, name_len, lnum, offs, len);

	return 0;
}

static int add_xattr(struct ubifs_ino_node *host_ino, struct stat *st, ino_t inum,
		     char *name, const void *data, unsigned int data_len)
{
	struct ubifs_ino_node *ino;
	struct ubifs_dent_node *xent;
	struct qstr nm;
	union ubifs_key xkey, nkey;
	int len, ret;

	nm.name = name;
	nm.len = strlen(name);

	host_ino->xattr_cnt++;
	host_ino->xattr_size += CALC_DENT_SIZE(nm.len);
	host_ino->xattr_size += CALC_XATTR_BYTES(data_len);
	host_ino->xattr_names += nm.len;

	xent = xzalloc(sizeof(*xent) + nm.len + 1);
	ino = xzalloc(sizeof(*ino) + data_len);

	xent_key_init(c, &xkey, inum, &nm);
	xent->ch.node_type = UBIFS_XENT_NODE;
	key_write(&xkey, &xent->key);

	len = UBIFS_XENT_NODE_SZ + nm.len + 1;

	xent->ch.len = len;
	xent->padding1 = 0;
	xent->type = UBIFS_ITYPE_DIR;
	xent->nlen = cpu_to_le16(nm.len);

	memcpy(xent->name, nm.name, nm.len + 1);

	inum = ++c->highest_inum;
	creat_sqnum = ++c->max_sqnum;

	xent->inum = cpu_to_le64(inum);

	ret = add_node(&xkey, nm.name, nm.len, xent, len);
	if (ret)
		goto out;

	ino->creat_sqnum = cpu_to_le64(creat_sqnum);
	ino->nlink      = cpu_to_le32(1);
	/*
	 * The time fields are updated assuming the default time granularity
	 * of 1 second. To support finer granularities, utime() would be needed.
	 */
	ino->atime_sec  = cpu_to_le64(st->st_atime);
	ino->ctime_sec  = cpu_to_le64(st->st_ctime);
	ino->mtime_sec  = cpu_to_le64(st->st_mtime);
	ino->atime_nsec = 0;
	ino->ctime_nsec = 0;
	ino->mtime_nsec = 0;
	ino->uid        = cpu_to_le32(st->st_uid);
	ino->gid        = cpu_to_le32(st->st_gid);
	ino->compr_type = cpu_to_le16(c->default_compr);
	ino->ch.node_type = UBIFS_INO_NODE;

	ino_key_init(&nkey, inum);
	key_write(&nkey, &ino->key);

	ino->size       = cpu_to_le64(data_len);
	ino->mode       = cpu_to_le32(S_IFREG);
	ino->data_len   = cpu_to_le32(data_len);
	ino->flags      = cpu_to_le32(UBIFS_XATTR_FL);

	if (data_len)
		memcpy(&ino->data, data, data_len);

	ret = add_node(&nkey, NULL, 0, ino, UBIFS_INO_NODE_SZ + data_len);

out:
	free(xent);
	free(ino);

	return ret;
}

#ifdef WITHOUT_XATTR
static inline int create_inum_attr(ino_t inum, const char *name)
{
	(void)inum;
	(void)name;

	return 0;
}

static inline int inode_add_xattr(struct ubifs_ino_node *host_ino,
				  const char *path_name,
				  struct stat *st, ino_t inum)
{
	(void)host_ino;
	(void)path_name;
	(void)st;
	(void)inum;

	return 0;
}
#else
static int create_inum_attr(ino_t inum, const char *name)
{
	char *str;
	int ret;

	if (!do_create_inum_attr)
		return 0;

	ret = asprintf(&str, "%llu", (unsigned long long)inum);
	if (ret < 0)
		return ret;

	ret = lsetxattr(name, "user.image-inode-number", str, ret, 0);

	free(str);

	return ret;
}

static int inode_add_xattr(struct ubifs_ino_node *host_ino,
			   const char *path_name, struct stat *st, ino_t inum)
{
	int ret;
	void *buf = NULL;
	ssize_t len;
	ssize_t pos = 0;

	len = llistxattr(path_name, NULL, 0);
	if (len < 0) {
		if (errno == ENOENT || errno == EOPNOTSUPP)
			return 0;

		sys_err_msg("llistxattr failed on %s", path_name);

		return len;
	}

	if (len == 0)
		goto noxattr;

	buf = xmalloc(len);

	len = llistxattr(path_name, buf, len);
	if (len < 0) {
		sys_err_msg("llistxattr failed on %s", path_name);
		goto out_free;
	}

	while (pos < len) {
		char attrbuf[1024] = { };
		char *name;
		ssize_t attrsize;

		name = buf + pos;
		pos += strlen(name) + 1;

		attrsize = lgetxattr(path_name, name, attrbuf, sizeof(attrbuf) - 1);
		if (attrsize < 0) {
			sys_err_msg("lgetxattr failed on %s", path_name);
			goto out_free;
		}

		if (!strcmp(name, "user.image-inode-number")) {
			ino_t inum_from_xattr;

			inum_from_xattr = strtoull(attrbuf, NULL, 10);
			if (inum != inum_from_xattr) {
				errno = EINVAL;
				sys_err_msg("calculated inum (%llu) doesn't match inum from xattr (%llu) size (%zd) on %s",
					    (unsigned long long)inum,
					    (unsigned long long)inum_from_xattr,
					    attrsize,
					    path_name);
				goto out_free;
			}

			continue;
		}

		ret = add_xattr(host_ino, st, inum, name, attrbuf, attrsize);
		if (ret < 0)
			goto out_free;
	}

noxattr:
	free(buf);
	return 0;

out_free:
	free(buf);

	return -1;
}
#endif

#ifdef WITH_SELINUX
static int inode_add_selinux_xattr(struct ubifs_ino_node *host_ino,
			   const char *path_name, struct stat *st, ino_t inum)
{
	int ret;
	char *sepath = NULL;
	char *name;
	struct qstr nm;
	unsigned int con_size;

	if (!context || !sehnd) {
		secontext = NULL;
		con_size = 0;
		return 0;
	}

	if (path_name[strlen(root)] == '/')
		sepath = strdup(&path_name[strlen(root)]);

	else if (asprintf(&sepath, "/%s", &path_name[strlen(root)]) < 0)
		sepath = NULL;

	if (!sepath)
		return sys_err_msg("could not get sepath\n");

	if (selabel_lookup(sehnd, &secontext, sepath, st->st_mode) < 0) {
		/* Failed to lookup context, assume unlabeled */
		secontext = strdup("system_u:object_r:unlabeled_t:s0");
		dbg_msg(2, "missing context: %s\t%s\t%d\n", secontext, sepath,
				st->st_mode);
	}

	dbg_msg(2, "appling selinux context on sepath=%s, secontext=%s\n",
			sepath, secontext);
	free(sepath);
	con_size = strlen(secontext) + 1;
	name = strdup(XATTR_NAME_SELINUX);

	nm.name = name;
	nm.len = strlen(name);
	host_ino->xattr_cnt++;
	host_ino->xattr_size += CALC_DENT_SIZE(nm.len);
	host_ino->xattr_size += CALC_XATTR_BYTES(con_size);
	host_ino->xattr_names += nm.len;

	ret = add_xattr(st, inum, secontext, con_size, &nm);
	if (ret < 0)
		dbg_msg(2, "add_xattr failed %d\n", ret);
	return ret;
}

#else
static inline int inode_add_selinux_xattr(struct ubifs_ino_node *host_ino,
			   const char *path_name, struct stat *st, ino_t inum)
{
	(void)host_ino;
	(void)path_name;
	(void)st;
	(void)inum;

	return 0;
}
#endif

static int set_fscrypt_context(struct ubifs_ino_node *host_ino, ino_t inum,
			       struct stat *host_st,
			       struct fscrypt_context *fctx)
{
	return add_xattr(host_ino, host_st, inum,
			 xstrdup(UBIFS_XATTR_NAME_ENCRYPTION_CONTEXT),
			 fctx, sizeof(*fctx));
}

static int encrypt_symlink(void *dst, void *data, unsigned int data_len,
			   struct fscrypt_context *fctx)
{
	struct fscrypt_symlink_data *sd;
	void *inbuf, *outbuf, *crypt_key;
	unsigned int max_namelen = UBIFS_MAX_INO_DATA;
	unsigned int padding = 4 << (fctx->flags & FS_POLICY_FLAGS_PAD_MASK);
	unsigned int cryptlen;
	unsigned int link_disk_len = fscrypt_fname_encrypted_size(fctx, data_len) + sizeof(struct fscrypt_symlink_data);

	cryptlen = max_t(unsigned int, data_len, FS_CRYPTO_BLOCK_SIZE);
	cryptlen = round_up(cryptlen, padding);
	cryptlen = min(cryptlen, max_namelen);

	sd = xzalloc(link_disk_len);
	inbuf = xmalloc(cryptlen);
	/* CTS mode needs a block size aligned buffer */
	outbuf = xmalloc(round_up(cryptlen, FS_CRYPTO_BLOCK_SIZE));

	memset(inbuf, 0, cryptlen);
	memcpy(inbuf, data, data_len);

	crypt_key = calc_fscrypt_subkey(fctx);
	if (!crypt_key)
		return err_msg("could not compute subkey");
	if (encrypt_aes128_cbc_cts(inbuf, cryptlen, crypt_key, outbuf) < 0)
		return err_msg("could not encrypt filename");

	memcpy(sd->encrypted_path, outbuf, cryptlen);
	sd->len = cpu_to_le16(cryptlen);
	memcpy(dst, sd, link_disk_len);
	((char *)dst)[link_disk_len - 1] = '\0';

	free(crypt_key);
	free(inbuf);
	free(outbuf);
	free(sd);
	return link_disk_len;
}

/**
 * add_inode - write an inode.
 * @st: stat information of source inode
 * @inum: target inode number
 * @data: inode data (for special inodes e.g. symlink path etc)
 * @data_len: inode data length
 * @flags: source inode flags
 */
static int add_inode(struct stat *st, ino_t inum, void *data,
		     unsigned int data_len, int flags, const char *xattr_path,
		     struct fscrypt_context *fctx)
{
	struct ubifs_ino_node *ino = node_buf;
	union ubifs_key key;
	int len, use_flags = 0, ret;

	if (c->default_compr != UBIFS_COMPR_NONE)
		use_flags |= UBIFS_COMPR_FL;
	if (flags & FS_COMPR_FL)
		use_flags |= UBIFS_COMPR_FL;
	if (flags & FS_SYNC_FL)
		use_flags |= UBIFS_SYNC_FL;
	if (flags & FS_IMMUTABLE_FL)
		use_flags |= UBIFS_IMMUTABLE_FL;
	if (flags & FS_APPEND_FL)
		use_flags |= UBIFS_APPEND_FL;
	if (flags & FS_DIRSYNC_FL && S_ISDIR(st->st_mode))
		use_flags |= UBIFS_DIRSYNC_FL;
	if (fctx)
		use_flags |= UBIFS_CRYPT_FL;
	memset(ino, 0, UBIFS_INO_NODE_SZ);

	ino_key_init(&key, inum);
	ino->ch.node_type = UBIFS_INO_NODE;
	key_write(&key, &ino->key);
	ino->creat_sqnum = cpu_to_le64(creat_sqnum);
	ino->size       = cpu_to_le64(st->st_size);
	ino->nlink      = cpu_to_le32(st->st_nlink);
	/*
	 * The time fields are updated assuming the default time granularity
	 * of 1 second. To support finer granularities, utime() would be needed.
	 */
	ino->atime_sec  = cpu_to_le64(st->st_atime);
	ino->ctime_sec  = cpu_to_le64(st->st_ctime);
	ino->mtime_sec  = cpu_to_le64(st->st_mtime);
	ino->atime_nsec = 0;
	ino->ctime_nsec = 0;
	ino->mtime_nsec = 0;
	ino->uid        = cpu_to_le32(st->st_uid);
	ino->gid        = cpu_to_le32(st->st_gid);
	ino->mode       = cpu_to_le32(st->st_mode);
	ino->flags      = cpu_to_le32(use_flags);
	ino->compr_type = cpu_to_le16(c->default_compr);
	if (data_len) {
		if (!S_ISLNK(st->st_mode))
			return err_msg("Expected symlink");

		if (!fctx) {
			memcpy(&ino->data, data, data_len);
		} else {
			ret = encrypt_symlink(&ino->data, data, data_len, fctx);
			if (ret < 0)
				return ret;
			data_len = ret;
		}
	}
	ino->data_len   = cpu_to_le32(data_len);
	len = UBIFS_INO_NODE_SZ + data_len;

	if (xattr_path) {
#ifdef WITH_SELINUX
		ret = inode_add_selinux_xattr(ino, xattr_path, st, inum);
#else
		ret = inode_add_xattr(ino, xattr_path, st, inum);
#endif
		if (ret < 0)
			return ret;
	}

	if (fctx) {
		ret = set_fscrypt_context(ino, inum, st, fctx);
		if (ret < 0)
			return ret;
	}

	return add_node(&key, NULL, 0, ino, len);
}

/**
 * add_dir_inode - write an inode for a directory.
 * @dir: source directory
 * @inum: target inode number
 * @size: target directory size
 * @nlink: target directory link count
 * @st: struct stat object describing attributes (except size and nlink) of the
 *      target inode to create
 *
 * Note, this function may be called with %NULL @dir, when the directory which
 * is being created does not exist at the host file system, but is defined by
 * the device table.
 */
static int add_dir_inode(const char *path_name, DIR *dir, ino_t inum, loff_t size,
			 unsigned int nlink, struct stat *st,
			 struct fscrypt_context *fctx)
{
	int fd, flags = 0;

	st->st_size = size;
	st->st_nlink = nlink;

	if (dir) {
		fd = dirfd(dir);
		if (fd == -1)
			return sys_err_msg("dirfd failed");
		if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == -1)
			flags = 0;
	}

	return add_inode(st, inum, NULL, 0, flags, path_name, fctx);
}

/**
 * add_dev_inode - write an inode for a character or block device.
 * @st: stat information of source inode
 * @inum: target inode number
 * @flags: source inode flags
 */
static int add_dev_inode(const char *path_name, struct stat *st, ino_t inum, int flags)
{
	union ubifs_dev_desc dev;

	dev.huge = cpu_to_le64(makedev(major(st->st_rdev), minor(st->st_rdev)));
	return add_inode(st, inum, &dev, 8, flags, path_name, NULL);
}

/**
 * add_symlink_inode - write an inode for a symbolic link.
 * @path_name: path name of symbolic link inode itself (not the link target)
 * @st: stat information of source inode
 * @inum: target inode number
 * @flags: source inode flags
 */
static int add_symlink_inode(const char *path_name, struct stat *st, ino_t inum,
			     int flags, struct fscrypt_context *fctx)
{
	char buf[UBIFS_MAX_INO_DATA + 2];
	ssize_t len;

	/* Take the symlink as is */
	len = readlink(path_name, buf, UBIFS_MAX_INO_DATA + 1);
	if (len <= 0)
		return sys_err_msg("readlink failed for %s", path_name);
	if (len > UBIFS_MAX_INO_DATA)
		return err_msg("symlink too long for %s", path_name);

	return add_inode(st, inum, buf, len, flags, path_name, fctx);
}

static void set_dent_cookie(struct ubifs_dent_node *dent)
{
	if (c->double_hash)
		RAND_bytes((void *)&dent->cookie, sizeof(dent->cookie));
	else
		dent->cookie = 0;
}

/**
 * add_dent_node - write a directory entry node.
 * @dir_inum: target inode number of directory
 * @name: directory entry name
 * @inum: target inode number of the directory entry
 * @type: type of the target inode
 */
static int add_dent_node(ino_t dir_inum, const char *name, ino_t inum,
			 unsigned char type, struct fscrypt_context *fctx)
{
	struct ubifs_dent_node *dent = node_buf;
	union ubifs_key key;
	struct qstr dname;
	char *kname;
	int kname_len;
	int len;

	dbg_msg(3, "%s ino %lu type %u dir ino %lu", name, (unsigned long)inum,
		(unsigned int)type, (unsigned long)dir_inum);
	memset(dent, 0, UBIFS_DENT_NODE_SZ);

	dname.name = (void *)name;
	dname.len = strlen(name);

	dent->ch.node_type = UBIFS_DENT_NODE;

	dent->inum = cpu_to_le64(inum);
	dent->padding1 = 0;
	dent->type = type;
	set_dent_cookie(dent);

	if (!fctx) {
		dent_key_init(c, &key, dir_inum, dname.name, dname.len);
		dent->nlen = cpu_to_le16(dname.len);
		memcpy(dent->name, dname.name, dname.len);
		dent->name[dname.len] = '\0';
		len = UBIFS_DENT_NODE_SZ + dname.len + 1;

		kname_len = dname.len;
		kname = strdup(name);
		if (!kname)
			return err_msg("cannot allocate memory");
	} else {
		void *inbuf, *outbuf, *crypt_key;
		unsigned int max_namelen = type == UBIFS_ITYPE_LNK ? UBIFS_MAX_INO_DATA : UBIFS_MAX_NLEN;
		unsigned int padding = 4 << (fctx->flags & FS_POLICY_FLAGS_PAD_MASK);
		unsigned int cryptlen;

		cryptlen = max_t(unsigned int, dname.len, FS_CRYPTO_BLOCK_SIZE);
		cryptlen = round_up(cryptlen, padding);
		cryptlen = min(cryptlen, max_namelen);

		inbuf = xmalloc(cryptlen);
		/* CTS mode needs a block size aligned buffer */
		outbuf = xmalloc(round_up(cryptlen, FS_CRYPTO_BLOCK_SIZE));

		memset(inbuf, 0, cryptlen);
		memcpy(inbuf, dname.name, dname.len);

		crypt_key = calc_fscrypt_subkey(fctx);
		if (!crypt_key)
			return err_msg("could not compute subkey");
		if (encrypt_aes128_cbc_cts(inbuf, cryptlen, crypt_key, outbuf) < 0)
			return err_msg("could not encrypt filename");

		dent->nlen = cpu_to_le16(cryptlen);
		memcpy(dent->name, outbuf, cryptlen);
		dent->name[cryptlen] = '\0';
		len = UBIFS_DENT_NODE_SZ + cryptlen + 1;

		dent_key_init(c, &key, dir_inum, outbuf, cryptlen);

		kname_len = cryptlen;
		kname = xmalloc(cryptlen);
		memcpy(kname, outbuf, cryptlen);
		free(crypt_key);
		free(inbuf);
		free(outbuf);
	}
	key_write(&key, dent->key);

	return add_node(&key, kname, kname_len, dent, len);
}

/**
 * lookup_inum_mapping - add an inode mapping for link counting.
 * @dev: source device on which source inode number resides
 * @inum: source inode number
 */
static struct inum_mapping *lookup_inum_mapping(dev_t dev, ino_t inum)
{
	struct inum_mapping *im;
	unsigned int k;

	k = inum % HASH_TABLE_SIZE;
	im = hash_table[k];
	while (im) {
		if (im->dev == dev && im->inum == inum)
			return im;
		im = im->next;
	}
	im = xmalloc(sizeof(struct inum_mapping));
	im->next = hash_table[k];
	im->prev = NULL;
	im->dev = dev;
	im->inum = inum;
	im->use_inum = 0;
	im->use_nlink = 0;
	if (hash_table[k])
		hash_table[k]->prev = im;
	hash_table[k] = im;
	return im;
}

/**
 * all_zero - does a buffer contain only zero bytes.
 * @buf: buffer
 * @len: buffer length
 */
static int all_zero(void *buf, int len)
{
	unsigned char *p = buf;

	while (len--)
		if (*p++ != 0)
			return 0;
	return 1;
}

/**
 * add_file - write the data of a file and its inode to the output file.
 * @path_name: source path name
 * @st: source inode stat information
 * @inum: target inode number
 * @flags: source inode flags
 */
static int add_file(const char *path_name, struct stat *st, ino_t inum,
		    int flags, struct fscrypt_context *fctx)
{
	struct ubifs_data_node *dn = node_buf;
	void *buf = block_buf;
	loff_t file_size = 0;
	ssize_t ret, bytes_read;
	union ubifs_key key;
	int fd, dn_len, err, compr_type, use_compr;
	unsigned int block_no = 0;
	size_t out_len;

	fd = open(path_name, O_RDONLY | O_LARGEFILE);
	if (fd == -1)
		return sys_err_msg("failed to open file '%s'", path_name);
	do {
		/* Read next block */
		bytes_read = 0;
		do {
			ret = read(fd, buf + bytes_read,
				   UBIFS_BLOCK_SIZE - bytes_read);
			if (ret == -1) {
				sys_err_msg("failed to read file '%s'",
					    path_name);
				close(fd);
				return 1;
			}
			bytes_read += ret;
		} while (ret != 0 && bytes_read != UBIFS_BLOCK_SIZE);
		if (bytes_read == 0)
			break;
		file_size += bytes_read;
		/* Skip holes */
		if (all_zero(buf, bytes_read)) {
			block_no += 1;
			continue;
		}
		/* Make data node */
		memset(dn, 0, UBIFS_DATA_NODE_SZ);
		data_key_init(&key, inum, block_no);
		dn->ch.node_type = UBIFS_DATA_NODE;
		key_write(&key, &dn->key);
		out_len = NODE_BUFFER_SIZE - UBIFS_DATA_NODE_SZ;
		if (c->default_compr == UBIFS_COMPR_NONE &&
		    (flags & FS_COMPR_FL))
#ifdef WITHOUT_LZO
			use_compr = UBIFS_COMPR_ZLIB;
#else
			use_compr = UBIFS_COMPR_LZO;
#endif
		else
			use_compr = c->default_compr;
		compr_type = compress_data(buf, bytes_read, &dn->data,
					   &out_len, use_compr);
		dn->compr_type = cpu_to_le16(compr_type);
		dn->size = cpu_to_le32(bytes_read);

		if (!fctx) {
			dn->compr_size = 0;
		} else {
			void *inbuf, *outbuf, *crypt_key;
			size_t ret, pad_len = round_up(out_len, FS_CRYPTO_BLOCK_SIZE);

			dn->compr_size = out_len;

			inbuf = xzalloc(pad_len);
			outbuf = xzalloc(pad_len);

			memcpy(inbuf, &dn->data, out_len);

			crypt_key = calc_fscrypt_subkey(fctx);
			if (!crypt_key)
				return err_msg("could not compute subkey");

			ret = encrypt_block_aes128_cbc(inbuf, pad_len, crypt_key, block_no,
						       outbuf);
			if (ret != pad_len)
				return err_msg("encrypt_block_aes128_cbc returned %zi instead of %zi", ret, pad_len);

			memcpy(&dn->data, outbuf, pad_len);

			out_len = pad_len;

			free(inbuf);
			free(outbuf);
			free(crypt_key);
		}

		dn_len = UBIFS_DATA_NODE_SZ + out_len;
		/* Add data node to file system */
		err = add_node(&key, NULL, 0, dn, dn_len);
		if (err) {
			close(fd);
			return err;
		}

		block_no++;
	} while (ret != 0);

	if (close(fd) == -1)
		return sys_err_msg("failed to close file '%s'", path_name);
	if (file_size != st->st_size)
		return err_msg("file size changed during writing file '%s'",
			       path_name);

	return add_inode(st, inum, NULL, 0, flags, path_name, fctx);
}

/**
 * add_non_dir - write a non-directory to the output file.
 * @path_name: source path name
 * @inum: target inode number is passed and returned here (due to link counting)
 * @nlink: number of links if known otherwise zero
 * @type: UBIFS inode type is returned here
 * @st: struct stat object containing inode attributes which should be use when
 *      creating the UBIFS inode
 */
static int add_non_dir(const char *path_name, ino_t *inum, unsigned int nlink,
		       unsigned char *type, struct stat *st,
		       struct fscrypt_context *fctx)
{
	int fd, flags = 0;

	dbg_msg(2, "%s", path_name);

	if (S_ISREG(st->st_mode)) {
		fd = open(path_name, O_RDONLY);
		if (fd == -1)
			return sys_err_msg("failed to open file '%s'",
					   path_name);
		if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == -1)
			flags = 0;
		if (close(fd) == -1)
			return sys_err_msg("failed to close file '%s'",
					   path_name);
		*type = UBIFS_ITYPE_REG;
	} else if (S_ISCHR(st->st_mode))
		*type = UBIFS_ITYPE_CHR;
	else if (S_ISBLK(st->st_mode))
		*type = UBIFS_ITYPE_BLK;
	else if (S_ISLNK(st->st_mode))
		*type = UBIFS_ITYPE_LNK;
	else if (S_ISSOCK(st->st_mode))
		*type = UBIFS_ITYPE_SOCK;
	else if (S_ISFIFO(st->st_mode))
		*type = UBIFS_ITYPE_FIFO;
	else
		return err_msg("file '%s' has unknown inode type", path_name);

	if (nlink)
		st->st_nlink = nlink;
	else if (st->st_nlink > 1) {
		/*
		 * If the number of links is greater than 1, then add this file
		 * later when we know the number of links that we actually have.
		 * For now, we just put the inode mapping in the hash table.
		 */
		struct inum_mapping *im;

		im = lookup_inum_mapping(st->st_dev, st->st_ino);
		if (!im)
			return err_msg("out of memory");
		if (im->use_nlink == 0) {
			/* New entry */
			im->use_inum = *inum;
			im->use_nlink = 1;
			im->path_name = xmalloc(strlen(path_name) + 1);
			strcpy(im->path_name, path_name);
		} else {
			/* Existing entry */
			*inum = im->use_inum;
			im->use_nlink += 1;
			/* Return unused inode number */
			c->highest_inum -= 1;
		}

		memcpy(&im->st, st, sizeof(struct stat));
		return 0;
	} else
		st->st_nlink = 1;

	creat_sqnum = ++c->max_sqnum;

	if (S_ISREG(st->st_mode))
		return add_file(path_name, st, *inum, flags, fctx);
	if (S_ISCHR(st->st_mode))
		return add_dev_inode(path_name, st, *inum, flags);
	if (S_ISBLK(st->st_mode))
		return add_dev_inode(path_name, st, *inum, flags);
	if (S_ISLNK(st->st_mode))
		return add_symlink_inode(path_name, st, *inum, flags, fctx);
	if (S_ISSOCK(st->st_mode))
		return add_inode(st, *inum, NULL, 0, flags, NULL, NULL);
	if (S_ISFIFO(st->st_mode))
		return add_inode(st, *inum, NULL, 0, flags, NULL, NULL);

	return err_msg("file '%s' has unknown inode type", path_name);
}

/**
 * add_directory - write a directory tree to the output file.
 * @dir_name: directory path name
 * @dir_inum: UBIFS inode number of directory
 * @st: directory inode statistics
 * @existing: zero if this function is called for a directory which
 *            does not exist on the host file-system and it is being
 *            created because it is defined in the device table file.
 */
static int add_directory(const char *dir_name, ino_t dir_inum, struct stat *st,
			 int existing, struct fscrypt_context *fctx)
{
	struct dirent *entry;
	DIR *dir = NULL;
	int err = 0;
	loff_t size = UBIFS_INO_NODE_SZ;
	char *name = NULL;
	unsigned int nlink = 2;
	struct path_htbl_element *ph_elt;
	struct name_htbl_element *nh_elt = NULL;
	struct hashtable_itr *itr;
	ino_t inum;
	unsigned char type;
	unsigned long long dir_creat_sqnum = ++c->max_sqnum;

	dbg_msg(2, "%s", dir_name);
	if (existing) {
		dir = opendir(dir_name);
		if (dir == NULL)
			return sys_err_msg("cannot open directory '%s'",
					   dir_name);
	}

	/*
	 * Check whether this directory contains files which should be
	 * added/changed because they were specified in the device table.
	 * @ph_elt will be non-zero if yes.
	 */
	ph_elt = devtbl_find_path(dir_name + root_len - 1);

	/*
	 * Before adding the directory itself, we have to iterate over all the
	 * entries the device table adds to this directory and create them.
	 */
	for (; existing;) {
		struct stat dent_st;
		struct fscrypt_context *new_fctx = NULL;

		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			if (errno == 0)
				break;
			sys_err_msg("error reading directory '%s'", dir_name);
			err = -1;
			break;
		}

		if (strcmp(".", entry->d_name) == 0)
			continue;
		if (strcmp("..", entry->d_name) == 0)
			continue;

		if (ph_elt)
			/*
			 * This directory was referred to at the device table
			 * file. Check if this directory entry is referred at
			 * too.
			 */
			nh_elt = devtbl_find_name(ph_elt, entry->d_name);

		/*
		 * We are going to create the file corresponding to this
		 * directory entry (@entry->d_name). We use 'struct stat'
		 * object to pass information about file attributes (actually
		 * only about UID, GID, mode, major, and minor). Get attributes
		 * for this file from the UBIFS rootfs on the host.
		 */
		free(name);
		name = make_path(dir_name, entry->d_name);
		if (lstat(name, &dent_st) == -1) {
			sys_err_msg("lstat failed for file '%s'", name);
			goto out_free;
		}

		if (squash_owner)
			/*
			 * Squash UID/GID. But the device table may override
			 * this.
			 */
			dent_st.st_uid = dent_st.st_gid = 0;

		/*
		 * And if the device table describes the same file, override
		 * the attributes. However, this is not allowed for device node
		 * files.
		 */
		if (nh_elt && override_attributes(&dent_st, ph_elt, nh_elt))
			goto out_free;

		inum = ++c->highest_inum;

		new_fctx = inherit_fscrypt_context(fctx);

		if (S_ISDIR(dent_st.st_mode)) {
			err = add_directory(name, inum, &dent_st, 1, new_fctx);
			if (err)
				goto out_free;
			nlink += 1;
			type = UBIFS_ITYPE_DIR;
		} else {
			err = add_non_dir(name, &inum, 0, &type, &dent_st, new_fctx);
			if (err)
				goto out_free;
		}

		err = create_inum_attr(inum, name);
		if (err)
			goto out_free;

		err = add_dent_node(dir_inum, entry->d_name, inum, type, fctx);
		if (err)
			goto out_free;
		size += ALIGN(UBIFS_DENT_NODE_SZ + strlen(entry->d_name) + 1,
			      8);

		free_fscrypt_context(new_fctx);
	}

	/*
	 * OK, we have created all files in this directory (recursively), let's
	 * also create all files described in the device table. All t
	 */
	nh_elt = first_name_htbl_element(ph_elt, &itr);
	while (nh_elt) {
		struct stat fake_st;
		struct fscrypt_context *new_fctx = NULL;

		/*
		 * We prohibit creating regular files using the device table,
		 * the device table may only re-define attributes of regular
		 * files.
		 */
		if (S_ISREG(nh_elt->mode)) {
			err_msg("Bad device table entry %s/%s - it is "
				"prohibited to create regular files "
				"via device table",
				strcmp(ph_elt->path, "/") ? ph_elt->path : "",
				nh_elt->name);
			goto out_free;
		}

		memcpy(&fake_st, &root_st, sizeof(struct stat));
		fake_st.st_uid  = nh_elt->uid;
		fake_st.st_gid  = nh_elt->gid;
		fake_st.st_mode = nh_elt->mode;
		fake_st.st_rdev = nh_elt->dev;
		fake_st.st_nlink = 1;

		free(name);
		name = make_path(dir_name, nh_elt->name);
		inum = ++c->highest_inum;

		new_fctx = inherit_fscrypt_context(fctx);

		if (S_ISDIR(nh_elt->mode)) {
			err = add_directory(name, inum, &fake_st, 0, new_fctx);
			if (err)
				goto out_free;
			nlink += 1;
			type = UBIFS_ITYPE_DIR;
		} else {
			err = add_non_dir(name, &inum, 0, &type, &fake_st, new_fctx);
			if (err)
				goto out_free;
		}

		err = create_inum_attr(inum, name);
		if (err)
			goto out_free;

		err = add_dent_node(dir_inum, nh_elt->name, inum, type, fctx);
		if (err)
			goto out_free;
		size += ALIGN(UBIFS_DENT_NODE_SZ + strlen(nh_elt->name) + 1, 8);

		nh_elt = next_name_htbl_element(ph_elt, &itr);
		free_fscrypt_context(new_fctx);
	}

	creat_sqnum = dir_creat_sqnum;

	err = add_dir_inode(dir ? dir_name : NULL, dir, dir_inum, size, nlink, st, fctx);
	if (err)
		goto out_free;

	free(name);
	if (existing && closedir(dir) == -1)
		return sys_err_msg("error closing directory '%s'", dir_name);

	return 0;

out_free:
	free(name);
	if (existing)
		closedir(dir);
	return -1;
}

/**
 * add_multi_linked_files - write all the files for which we counted links.
 */
static int add_multi_linked_files(void)
{
	int i, err;

	for (i = 0; i < HASH_TABLE_SIZE; i++) {
		struct inum_mapping *im;
		unsigned char type = 0;

		for (im = hash_table[i]; im; im = im->next) {
			dbg_msg(2, "%s", im->path_name);
			err = add_non_dir(im->path_name, &im->use_inum,
					  im->use_nlink, &type, &im->st, NULL);
			if (err)
				return err;
		}
	}
	return 0;
}

/**
 * write_data - write the files and directories.
 */
static int write_data(void)
{
	int err;
	mode_t mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	struct path_htbl_element *ph_elt;
	struct name_htbl_element *nh_elt;

	if (root) {
		err = stat(root, &root_st);
		if (err)
			return sys_err_msg("bad root file-system directory '%s'",
					   root);
		if (squash_owner)
			root_st.st_uid = root_st.st_gid = 0;
	} else {
		root_st.st_mtime = time(NULL);
		root_st.st_atime = root_st.st_ctime = root_st.st_mtime;
		root_st.st_mode = mode;
	}

	/*
	 * Check for root entry and update permissions if it exists. This will
	 * also remove the entry from the device table list.
	 */
	ph_elt = devtbl_find_path("/");
	if (ph_elt) {
		nh_elt = devtbl_find_name(ph_elt, "");
		if (nh_elt && override_attributes(&root_st, ph_elt, nh_elt))
			return -1;
	}

	head_flags = 0;

	err = create_inum_attr(UBIFS_ROOT_INO, root);
	if (err)
		return err;

	err = add_directory(root, UBIFS_ROOT_INO, &root_st, !!root, root_fctx);
	if (err)
		return err;
	err = add_multi_linked_files();
	if (err)
		return err;
	return flush_nodes();
}

static int namecmp(const struct idx_entry *e1, const struct idx_entry *e2)
{
	size_t len1 = e1->name_len, len2 = e2->name_len;
	size_t clen = (len1 < len2) ? len1 : len2;
	int cmp;

	cmp = memcmp(e1->name, e2->name, clen);
	if (cmp)
		return cmp;
	return (len1 < len2) ? -1 : 1;
}

static int cmp_idx(const void *a, const void *b)
{
	const struct idx_entry *e1 = *(const struct idx_entry **)a;
	const struct idx_entry *e2 = *(const struct idx_entry **)b;
	int cmp;

	cmp = keys_cmp(&e1->key, &e2->key);
	if (cmp)
		return cmp;
	return namecmp(e1, e2);
}

/**
 * add_idx_node - write an index node to the head.
 * @node: index node
 * @child_cnt: number of children of this index node
 */
static int add_idx_node(void *node, int child_cnt)
{
	int err, lnum, offs, len;

	len = ubifs_idx_node_sz(c, child_cnt);

	prepare_node(node, len);

	err = reserve_space(len, &lnum, &offs);
	if (err)
		return err;

	memcpy(leb_buf + offs, node, len);
	memset(leb_buf + offs + len, 0xff, ALIGN(len, 8) - len);

	c->old_idx_sz += ALIGN(len, 8);

	dbg_msg(3, "at %d:%d len %d index size %llu", lnum, offs, len,
		c->old_idx_sz);

	/* The last index node written will be the root */
	c->zroot.lnum = lnum;
	c->zroot.offs = offs;
	c->zroot.len = len;

	return 0;
}

/**
 * write_index - write out the index.
 */
static int write_index(void)
{
	size_t sz, i, cnt, idx_sz, pstep, bcnt;
	struct idx_entry **idx_ptr, **p;
	struct ubifs_idx_node *idx;
	struct ubifs_branch *br;
	int child_cnt = 0, j, level, blnum, boffs, blen, blast_len, err;

	dbg_msg(1, "leaf node count: %zd", idx_cnt);

	/* Reset the head for the index */
	head_flags = LPROPS_INDEX;
	/* Allocate index node */
	idx_sz = ubifs_idx_node_sz(c, c->fanout);
	idx = xmalloc(idx_sz);
	/* Make an array of pointers to sort the index list */
	sz = idx_cnt * sizeof(struct idx_entry *);
	if (sz / sizeof(struct idx_entry *) != idx_cnt) {
		free(idx);
		return err_msg("index is too big (%zu entries)", idx_cnt);
	}
	idx_ptr = xmalloc(sz);
	idx_ptr[0] = idx_list_first;
	for (i = 1; i < idx_cnt; i++)
		idx_ptr[i] = idx_ptr[i - 1]->next;
	qsort(idx_ptr, idx_cnt, sizeof(struct idx_entry *), cmp_idx);
	/* Write level 0 index nodes */
	cnt = idx_cnt / c->fanout;
	if (idx_cnt % c->fanout)
		cnt += 1;
	p = idx_ptr;
	blnum = head_lnum;
	boffs = head_offs;
	for (i = 0; i < cnt; i++) {
		/*
		 * Calculate the child count. All index nodes are created full
		 * except for the last index node on each row.
		 */
		if (i == cnt - 1) {
			child_cnt = idx_cnt % c->fanout;
			if (child_cnt == 0)
				child_cnt = c->fanout;
		} else
			child_cnt = c->fanout;
		memset(idx, 0, idx_sz);
		idx->ch.node_type = UBIFS_IDX_NODE;
		idx->child_cnt = cpu_to_le16(child_cnt);
		idx->level = cpu_to_le16(0);
		for (j = 0; j < child_cnt; j++, p++) {
			br = ubifs_idx_branch(c, idx, j);
			key_write_idx(&(*p)->key, &br->key);
			br->lnum = cpu_to_le32((*p)->lnum);
			br->offs = cpu_to_le32((*p)->offs);
			br->len = cpu_to_le32((*p)->len);
		}
		add_idx_node(idx, child_cnt);
	}
	/* Write level 1 index nodes and above */
	level = 0;
	pstep = 1;
	while (cnt > 1) {
		/*
		 * 'blast_len' is the length of the last index node in the level
		 * below.
		 */
		blast_len = ubifs_idx_node_sz(c, child_cnt);
		/* 'bcnt' is the number of index nodes in the level below */
		bcnt = cnt;
		/* 'cnt' is the number of index nodes in this level */
		cnt = (cnt + c->fanout - 1) / c->fanout;
		if (cnt == 0)
			cnt = 1;
		level += 1;
		/*
		 * The key of an index node is the same as the key of its first
		 * child. Thus we can get the key by stepping along the bottom
		 * level 'p' with an increasing large step 'pstep'.
		 */
		p = idx_ptr;
		pstep *= c->fanout;
		for (i = 0; i < cnt; i++) {
			/*
			 * Calculate the child count. All index nodes are
			 * created full except for the last index node on each
			 * row.
			 */
			if (i == cnt - 1) {
				child_cnt = bcnt % c->fanout;
				if (child_cnt == 0)
					child_cnt = c->fanout;
			} else
				child_cnt = c->fanout;
			memset(idx, 0, idx_sz);
			idx->ch.node_type = UBIFS_IDX_NODE;
			idx->child_cnt = cpu_to_le16(child_cnt);
			idx->level = cpu_to_le16(level);
			for (j = 0; j < child_cnt; j++) {
				size_t bn = i * c->fanout + j;

				/*
				 * The length of the index node in the level
				 * below is 'idx_sz' except when it is the last
				 * node on the row. i.e. all the others on the
				 * row are full.
				 */
				if (bn == bcnt - 1)
					blen = blast_len;
				else
					blen = idx_sz;
				/*
				 * 'blnum' and 'boffs' hold the position of the
				 * index node on the level below.
				 */
				if (boffs + blen > c->leb_size) {
					blnum += 1;
					boffs = 0;
				}
				/*
				 * Fill in the branch with the key and position
				 * of the index node from the level below.
				 */
				br = ubifs_idx_branch(c, idx, j);
				key_write_idx(&(*p)->key, &br->key);
				br->lnum = cpu_to_le32(blnum);
				br->offs = cpu_to_le32(boffs);
				br->len = cpu_to_le32(blen);
				/*
				 * Step to the next index node on the level
				 * below.
				 */
				boffs += ALIGN(blen, 8);
				p += pstep;
			}
			add_idx_node(idx, child_cnt);
		}
	}

	/* Free stuff */
	for (i = 0; i < idx_cnt; i++) {
		free(idx_ptr[i]->name);
		free(idx_ptr[i]);
	}
	free(idx_ptr);
	free(idx);

	dbg_msg(1, "zroot is at %d:%d len %d", c->zroot.lnum, c->zroot.offs,
		c->zroot.len);

	/* Set the index head */
	c->ihead_lnum = head_lnum;
	c->ihead_offs = ALIGN(head_offs, c->min_io_size);
	dbg_msg(1, "ihead is at %d:%d", c->ihead_lnum, c->ihead_offs);

	/* Flush the last index LEB */
	err = flush_nodes();
	if (err)
		return err;

	return 0;
}

/**
 * set_gc_lnum - set the LEB number reserved for the garbage collector.
 */
static int set_gc_lnum(void)
{
	int err;

	c->gc_lnum = head_lnum++;
	err = write_empty_leb(c->gc_lnum);
	if (err)
		return err;
	set_lprops(c->gc_lnum, 0, 0);
	c->lst.empty_lebs += 1;
	return 0;
}

/**
 * finalize_leb_cnt - now that we know how many LEBs we used.
 */
static int finalize_leb_cnt(void)
{
	c->leb_cnt = head_lnum;
	if (c->leb_cnt > c->max_leb_cnt)
		return err_msg("max_leb_cnt too low (%d needed)", c->leb_cnt);
	c->main_lebs = c->leb_cnt - c->main_first;
	if (verbose) {
		printf("\tsuper lebs:   %d\n", UBIFS_SB_LEBS);
		printf("\tmaster lebs:  %d\n", UBIFS_MST_LEBS);
		printf("\tlog_lebs:     %d\n", c->log_lebs);
		printf("\tlpt_lebs:     %d\n", c->lpt_lebs);
		printf("\torph_lebs:    %d\n", c->orph_lebs);
		printf("\tmain_lebs:    %d\n", c->main_lebs);
		printf("\tgc lebs:      %d\n", 1);
		printf("\tindex lebs:   %d\n", c->lst.idx_lebs);
		printf("\tleb_cnt:      %d\n", c->leb_cnt);
	}
	dbg_msg(1, "total_free:  %llu", c->lst.total_free);
	dbg_msg(1, "total_dirty: %llu", c->lst.total_dirty);
	dbg_msg(1, "total_used:  %llu", c->lst.total_used);
	dbg_msg(1, "total_dead:  %llu", c->lst.total_dead);
	dbg_msg(1, "total_dark:  %llu", c->lst.total_dark);
	dbg_msg(1, "index size:  %llu", c->old_idx_sz);
	dbg_msg(1, "empty_lebs:  %d", c->lst.empty_lebs);
	return 0;
}

static int ubifs_format_version(void)
{
	if (c->double_hash || c->encrypted)
		return 5;

	/* Default */
	return 4;
}

/**
 * write_super - write the super block.
 */
static int write_super(void)
{
	struct ubifs_sb_node sup;

	memset(&sup, 0, UBIFS_SB_NODE_SZ);

	sup.ch.node_type  = UBIFS_SB_NODE;
	sup.key_hash      = c->key_hash_type;
	sup.min_io_size   = cpu_to_le32(c->min_io_size);
	sup.leb_size      = cpu_to_le32(c->leb_size);
	sup.leb_cnt       = cpu_to_le32(c->leb_cnt);
	sup.max_leb_cnt   = cpu_to_le32(c->max_leb_cnt);
	sup.max_bud_bytes = cpu_to_le64(c->max_bud_bytes);
	sup.log_lebs      = cpu_to_le32(c->log_lebs);
	sup.lpt_lebs      = cpu_to_le32(c->lpt_lebs);
	sup.orph_lebs     = cpu_to_le32(c->orph_lebs);
	sup.jhead_cnt     = cpu_to_le32(c->jhead_cnt);
	sup.fanout        = cpu_to_le32(c->fanout);
	sup.lsave_cnt     = cpu_to_le32(c->lsave_cnt);
	sup.fmt_version   = cpu_to_le32(ubifs_format_version());
	sup.default_compr = cpu_to_le16(c->default_compr);
	sup.rp_size       = cpu_to_le64(c->rp_size);
	sup.time_gran     = cpu_to_le32(DEFAULT_TIME_GRAN);
	uuid_generate_random(sup.uuid);
	if (verbose) {
		char s[40];

		uuid_unparse_upper(sup.uuid, s);
		printf("\tUUID:         %s\n", s);
	}
	if (c->big_lpt)
		sup.flags |= cpu_to_le32(UBIFS_FLG_BIGLPT);
	if (c->space_fixup)
		sup.flags |= cpu_to_le32(UBIFS_FLG_SPACE_FIXUP);
	if (c->double_hash)
		sup.flags |= cpu_to_le32(UBIFS_FLG_DOUBLE_HASH);
	if (c->encrypted)
		sup.flags |= cpu_to_le32(UBIFS_FLG_ENCRYPTION);

	return write_node(&sup, UBIFS_SB_NODE_SZ, UBIFS_SB_LNUM);
}

/**
 * write_master - write the master node.
 */
static int write_master(void)
{
	struct ubifs_mst_node mst;
	int err;

	memset(&mst, 0, UBIFS_MST_NODE_SZ);

	mst.ch.node_type = UBIFS_MST_NODE;
	mst.log_lnum     = cpu_to_le32(UBIFS_LOG_LNUM);
	mst.highest_inum = cpu_to_le64(c->highest_inum);
	mst.cmt_no       = cpu_to_le64(0);
	mst.flags        = cpu_to_le32(UBIFS_MST_NO_ORPHS);
	mst.root_lnum    = cpu_to_le32(c->zroot.lnum);
	mst.root_offs    = cpu_to_le32(c->zroot.offs);
	mst.root_len     = cpu_to_le32(c->zroot.len);
	mst.gc_lnum      = cpu_to_le32(c->gc_lnum);
	mst.ihead_lnum   = cpu_to_le32(c->ihead_lnum);
	mst.ihead_offs   = cpu_to_le32(c->ihead_offs);
	mst.index_size   = cpu_to_le64(c->old_idx_sz);
	mst.lpt_lnum     = cpu_to_le32(c->lpt_lnum);
	mst.lpt_offs     = cpu_to_le32(c->lpt_offs);
	mst.nhead_lnum   = cpu_to_le32(c->nhead_lnum);
	mst.nhead_offs   = cpu_to_le32(c->nhead_offs);
	mst.ltab_lnum    = cpu_to_le32(c->ltab_lnum);
	mst.ltab_offs    = cpu_to_le32(c->ltab_offs);
	mst.lsave_lnum   = cpu_to_le32(c->lsave_lnum);
	mst.lsave_offs   = cpu_to_le32(c->lsave_offs);
	mst.lscan_lnum   = cpu_to_le32(c->lscan_lnum);
	mst.empty_lebs   = cpu_to_le32(c->lst.empty_lebs);
	mst.idx_lebs     = cpu_to_le32(c->lst.idx_lebs);
	mst.total_free   = cpu_to_le64(c->lst.total_free);
	mst.total_dirty  = cpu_to_le64(c->lst.total_dirty);
	mst.total_used   = cpu_to_le64(c->lst.total_used);
	mst.total_dead   = cpu_to_le64(c->lst.total_dead);
	mst.total_dark   = cpu_to_le64(c->lst.total_dark);
	mst.leb_cnt      = cpu_to_le32(c->leb_cnt);

	err = write_node(&mst, UBIFS_MST_NODE_SZ, UBIFS_MST_LNUM);
	if (err)
		return err;

	err = write_node(&mst, UBIFS_MST_NODE_SZ, UBIFS_MST_LNUM + 1);
	if (err)
		return err;

	return 0;
}

/**
 * write_log - write an empty log.
 */
static int write_log(void)
{
	struct ubifs_cs_node cs;
	int err, i, lnum;

	lnum = UBIFS_LOG_LNUM;

	cs.ch.node_type = UBIFS_CS_NODE;
	cs.cmt_no = cpu_to_le64(0);

	err = write_node(&cs, UBIFS_CS_NODE_SZ, lnum);
	if (err)
		return err;

	lnum += 1;

	for (i = 1; i < c->log_lebs; i++, lnum++) {
		err = write_empty_leb(lnum);
		if (err)
			return err;
	}

	return 0;
}

/**
 * write_lpt - write the LEB properties tree.
 */
static int write_lpt(void)
{
	int err, lnum;

	err = create_lpt(c);
	if (err)
		return err;

	lnum = c->nhead_lnum + 1;
	while (lnum <= c->lpt_last) {
		err = write_empty_leb(lnum++);
		if (err)
			return err;
	}

	return 0;
}

/**
 * write_orphan_area - write an empty orphan area.
 */
static int write_orphan_area(void)
{
	int err, i, lnum;

	lnum = UBIFS_LOG_LNUM + c->log_lebs + c->lpt_lebs;
	for (i = 0; i < c->orph_lebs; i++, lnum++) {
		err = write_empty_leb(lnum);
		if (err)
			return err;
	}
	return 0;
}

/**
 * check_volume_empty - check if the UBI volume is empty.
 *
 * This function checks if the UBI volume is empty by looking if its LEBs are
 * mapped or not.
 *
 * Returns %0 in case of success, %1 is the volume is not empty,
 * and a negative error code in case of failure.
 */
static int check_volume_empty(void)
{
	int lnum, err;

	for (lnum = 0; lnum < c->vi.rsvd_lebs; lnum++) {
		err = ubi_is_mapped(out_fd, lnum);
		if (err < 0)
			return err;
		if (err == 1)
			return 1;
	}
	return 0;
}

/**
 * open_target - open the output target.
 *
 * Open the output target. The target can be an UBI volume
 * or a file.
 *
 * Returns %0 in case of success and %-1 in case of failure.
 */
static int open_target(void)
{
	if (out_ubi) {
		out_fd = open(output, O_RDWR | O_EXCL);

		if (out_fd == -1)
			return sys_err_msg("cannot open the UBI volume '%s'",
					   output);
		if (ubi_set_property(out_fd, UBI_VOL_PROP_DIRECT_WRITE, 1))
			return sys_err_msg("ubi_set_property failed");

		if (!yes && check_volume_empty()) {
			if (!prompt("UBI volume is not empty.  Format anyways?", false))
				return err_msg("UBI volume is not empty");
		}
	} else {
		out_fd = open(output, O_CREAT | O_RDWR | O_TRUNC,
			      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
		if (out_fd == -1)
			return sys_err_msg("cannot create output file '%s'",
					   output);
	}
	return 0;
}


/**
 * close_target - close the output target.
 *
 * Close the output target. If the target was an UBI
 * volume, also close libubi.
 *
 * Returns %0 in case of success and %-1 in case of failure.
 */
static int close_target(void)
{
	if (ubi)
		libubi_close(ubi);
	if (out_fd >= 0 && close(out_fd) == -1)
		return sys_err_msg("cannot close the target '%s'", output);
	if (output)
		free(output);
	return 0;
}

/**
 * init - initialize things.
 */
static int init(void)
{
	int err, i, main_lebs, big_lpt = 0, sz;

	c->highest_inum = UBIFS_FIRST_INO;

	c->jhead_cnt = 1;

	main_lebs = c->max_leb_cnt - UBIFS_SB_LEBS - UBIFS_MST_LEBS;
	main_lebs -= c->log_lebs + c->orph_lebs;

	err = calc_dflt_lpt_geom(c, &main_lebs, &big_lpt);
	if (err)
		return err;

	c->main_first = UBIFS_LOG_LNUM + c->log_lebs + c->lpt_lebs +
			c->orph_lebs;
	head_lnum = c->main_first;
	head_offs = 0;

	c->lpt_first = UBIFS_LOG_LNUM + c->log_lebs;
	c->lpt_last = c->lpt_first + c->lpt_lebs - 1;

	c->lpt = xmalloc(c->main_lebs * sizeof(struct ubifs_lprops));
	c->ltab = xmalloc(c->lpt_lebs * sizeof(struct ubifs_lprops));

	/* Initialize LPT's own lprops */
	for (i = 0; i < c->lpt_lebs; i++) {
		c->ltab[i].free = c->leb_size;
		c->ltab[i].dirty = 0;
	}

	c->dead_wm = ALIGN(MIN_WRITE_SZ, c->min_io_size);
	c->dark_wm = ALIGN(UBIFS_MAX_NODE_SZ, c->min_io_size);
	dbg_msg(1, "dead_wm %d  dark_wm %d", c->dead_wm, c->dark_wm);

	leb_buf = xmalloc(c->leb_size);
	node_buf = xmalloc(NODE_BUFFER_SIZE);
	block_buf = xmalloc(UBIFS_BLOCK_SIZE);

	sz = sizeof(struct inum_mapping *) * HASH_TABLE_SIZE;
	hash_table = xzalloc(sz);

	//TODO make this a parameter
	root_fctx = init_fscrypt_context();
	print_fscrypt_master_key_descriptor(root_fctx);
	c->double_hash = 1;
	c->encrypted = 1;

	err = init_compression();
	if (err)
		return err;

#ifdef WITH_SELINUX
	if (context) {
		struct selinux_opt seopts[] = {
			{ SELABEL_OPT_PATH, context }
		};

		sehnd = selabel_open(SELABEL_CTX_FILE, seopts, 1);
		if (!sehnd)
			return err_msg("could not open selinux context\n");
	}
#endif

	return 0;
}

static void destroy_hash_table(void)
{
	int i;

	for (i = 0; i < HASH_TABLE_SIZE; i++) {
		struct inum_mapping *im, *q;

		for (im = hash_table[i]; im; ) {
			q = im;
			im = im->next;
			free(q->path_name);
			free(q);
		}
	}
}

/**
 * deinit - deinitialize things.
 */
static void deinit(void)
{

#ifdef WITH_SELINUX
	if (sehnd)
		selabel_close(sehnd);
#endif

	free(c->lpt);
	free(c->ltab);
	free(leb_buf);
	free(node_buf);
	free(block_buf);
	destroy_hash_table();
	free(hash_table);
	destroy_compression();
	free_devtable_info();
}

/**
 * mkfs - make the file system.
 *
 * Each on-flash area has a corresponding function to create it. The order of
 * the functions reflects what information must be known to complete each stage.
 * As a consequence the output file is not written sequentially. No effort has
 * been made to make efficient use of memory or to allow for the possibility of
 * incremental updates to the output file.
 */
static int mkfs(void)
{
	int err = 0;

	err = init();
	if (err)
		goto out;

	err = write_data();
	if (err)
		goto out;

	err = set_gc_lnum();
	if (err)
		goto out;

	err = write_index();
	if (err)
		goto out;

	err = finalize_leb_cnt();
	if (err)
		goto out;

	err = write_lpt();
	if (err)
		goto out;

	err = write_super();
	if (err)
		goto out;

	err = write_master();
	if (err)
		goto out;

	err = write_log();
	if (err)
		goto out;

	err = write_orphan_area();

out:
	deinit();
	return err;
}

int main(int argc, char *argv[])
{
	int err;

	if (crypto_init())
		return -1;

	RAND_poll();

	err = get_options(argc, argv);
	if (err)
		return err;

	err = open_target();
	if (err)
		return err;

	err = mkfs();
	if (err) {
		close_target();
		return err;
	}

	err = close_target();
	if (err)
		return err;

	if (verbose)
		printf("Success!\n");

	crypto_cleanup();
	return 0;
}
