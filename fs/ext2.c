/*
 * This is a set of functions that provides minimal filesystem
 * functionality to the Linux bootstrapper.  All we can do is
 * open and read files... but that's all we need 8-)
 *
 * This file has been ported from the DEC 32-bit Linux version
 * by David Mosberger (davidm@cs.arizona.edu).
 */
#include <linux/stat.h>
#include <linux/types.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)

#  undef __KERNEL__
#  include <linux/ext2_fs.h>
#  define __KERNEL__
#  include <linux/fs.h>

#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)

#  undef __KERNEL__
#  include <linux/ext2_fs.h>
#  include <linux/fs.h>
#  define __KERNEL__

#else

#  include <linux/audit.h>
#  undef __KERNEL__
#  include <linux/ext2_fs.h>
#  include <linux/fs.h>
#  define __KERNEL__

#endif

#include "bootfs.h"
#include "cons.h"
#include "disklabel.h"
#include "utils.h"
#include "string.h"

#define MAX_OPEN_FILES		5

extern struct bootfs ext2fs;

static struct ext2_super_block sb;
static struct ext2_group_desc *gds;
static struct ext2_inode *root_inode = NULL;
static int ngroups = 0;
static int directlim;			/* Maximum direct blkno */
static int ind1lim;			/* Maximum single-indir blkno */
static int ind2lim;			/* Maximum double-indir blkno */
static int ptrs_per_blk;		/* ptrs/indirect block */
static char blkbuf[EXT2_MAX_BLOCK_SIZE];
static int cached_iblkno = -1;
static char iblkbuf[EXT2_MAX_BLOCK_SIZE];
static int cached_diblkno = -1;
static char diblkbuf[EXT2_MAX_BLOCK_SIZE];
static long dev = -1;
static long partition_offset;

static struct inode_table_entry {
	struct	ext2_inode	inode;
	int			inumber;
	int			free;
	unsigned short		old_mode;
} inode_table[MAX_OPEN_FILES];


/*
 * Initialize an ext2 partition starting at offset P_OFFSET; this is
 * sort-of the same idea as "mounting" it.  Read in the relevant
 * control structures and make them available to the user.  Returns 0
 * if successful, -1 on failure.
 */
static int ext2_mount(long cons_dev, long p_offset, long quiet)
{
	long sb_block = 1;
	long sb_offset;
	int i;

	dev = cons_dev;
	partition_offset = p_offset;

	/* initialize the inode table */
	for (i = 0; i < MAX_OPEN_FILES; i++) {
		inode_table[i].free = 1;
		inode_table[i].inumber = 0;
	}
	/* clear the root inode pointer (very important!) */
	root_inode = NULL;
	
	/* read in the first superblock */
	sb_offset = sb_block * EXT2_MIN_BLOCK_SIZE;
	if (cons_read(dev, &sb, sizeof(sb), partition_offset + sb_offset)
	    != sizeof(sb))
	{
		printf("ext2 sb read failed\n");
		return -1;
	}
	
	if (sb.s_magic != EXT2_SUPER_MAGIC) {
		if (!quiet) {
			printf("ext2_init: bad magic 0x%x\n", sb.s_magic);
		}
		return -1;
	}

	ngroups = (sb.s_blocks_count -
		   sb.s_first_data_block +
		   EXT2_BLOCKS_PER_GROUP(&sb) - 1)
		/ EXT2_BLOCKS_PER_GROUP(&sb);

	gds = (struct ext2_group_desc *)
	          malloc((size_t)(ngroups * sizeof(struct ext2_group_desc)));

	ext2fs.blocksize = EXT2_BLOCK_SIZE(&sb);

	/* read in the group descriptors (immediately follows superblock) */
	cons_read(dev, gds, ngroups * sizeof(struct ext2_group_desc),
		  partition_offset +
                  ext2fs.blocksize * (EXT2_MIN_BLOCK_SIZE/ext2fs.blocksize + 1));
	/*
	 * Calculate direct/indirect block limits for this file system
	 * (blocksize dependent):
	 */
	ext2fs.blocksize = EXT2_BLOCK_SIZE(&sb);
	directlim = EXT2_NDIR_BLOCKS - 1;
	ptrs_per_blk = ext2fs.blocksize/sizeof(unsigned int);
	ind1lim = ptrs_per_blk + directlim;
	ind2lim = (ptrs_per_blk * ptrs_per_blk) + directlim;

	return 0;
}


/*
 * Read the specified inode from the disk and return it to the user.
 * Returns NULL if the inode can't be read...
 */
static struct ext2_inode *ext2_iget(int ino)
{
	int i;
	struct ext2_inode *ip;
	struct inode_table_entry *itp = 0;
	int group;
	long offset;

	ip = 0;
	for (i = 0; i < MAX_OPEN_FILES; i++) {
#ifdef DEBUG_EXT2
		printf("ext2_iget: looping, entry %d inode %d free %d\n",
		       i, inode_table[i].inumber, inode_table[i].free);
#endif
		if (inode_table[i].free) {
			itp = &inode_table[i];
			ip = &itp->inode;
			break;
		}
	}
	if (!ip) {
		printf("ext2_iget: no free inodes\n");
		return NULL;
	}

	group = (ino-1) / sb.s_inodes_per_group;
#ifdef DEBUG_EXT2
	printf("group is %d\n", group);
#endif
	offset = partition_offset
		+ ((long) gds[group].bg_inode_table * (long)ext2fs.blocksize)
		+ (((ino - 1) % EXT2_INODES_PER_GROUP(&sb))
		   * EXT2_INODE_SIZE(&sb));
#ifdef DEBUG_EXT2
	printf("ext2_iget: reading %ld bytes at offset %ld "
	       "(%ld + (%d * %d) + ((%d) %% %d) * %d) "
	       "(inode %d -> table %d)\n", 
	       sizeof(struct ext2_inode), offset, partition_offset,
	       gds[group].bg_inode_table, ext2fs.blocksize,
	       ino - 1, EXT2_INODES_PER_GROUP(&sb), EXT2_INODE_SIZE(&sb),
	       ino, (int) (itp - inode_table));
#endif
	if (cons_read(dev, ip, sizeof(struct ext2_inode), offset) 
	    != sizeof(struct ext2_inode))
	{
		printf("ext2_iget: read error\n");
		return NULL;
	}

	itp->free = 0;
	itp->inumber = ino;
	itp->old_mode = ip->i_mode;

	return ip;
}


/*
 * Release our hold on an inode.  Since this is a read-only application,
 * don't worry about putting back any changes...
 */
static void ext2_iput(struct ext2_inode *ip)
{
	struct inode_table_entry *itp;

	/* Find and free the inode table slot we used... */
	itp = (struct inode_table_entry *)ip;

#ifdef DEBUG_EXT2
	printf("ext2_iput: inode %d table %d\n", itp->inumber,
	       (int) (itp - inode_table));
#endif
	itp->inumber = 0;
	itp->free = 1;
}


/*
 * Map a block offset into a file into an absolute block number.
 * (traverse the indirect blocks if necessary).  Note: Double-indirect
 * blocks allow us to map over 64Mb on a 1k file system.  Therefore, for
 * our purposes, we will NOT bother with triple indirect blocks.
 *
 * The "allocate" argument is set if we want to *allocate* a block
 * and we don't already have one allocated.
 */
static int ext2_blkno(struct ext2_inode *ip, int blkoff)
{
	unsigned int *lp;
	unsigned int *ilp;
	unsigned int *dlp;
	int blkno;
	int iblkno;
	int diblkno;
	unsigned long offset;

	ilp = (unsigned int *)iblkbuf;
	dlp = (unsigned int *)diblkbuf;
	lp = (unsigned int *)blkbuf;

	/* If it's a direct block, it's easy! */
	if (blkoff <= directlim) {
		return ip->i_block[blkoff];
	}

	/* Is it a single-indirect? */
	if (blkoff <= ind1lim) {
		iblkno = ip->i_block[EXT2_IND_BLOCK];

		if (iblkno == 0) {
			return 0;
		}

		/* Read the indirect block */
		if (cached_iblkno != iblkno) {
			offset = partition_offset + (long)iblkno * (long)ext2fs.blocksize;
			if (cons_read(dev, iblkbuf, ext2fs.blocksize, offset)
			    != ext2fs.blocksize)
			{
				printf("ext2_blkno: error on iblk read\n");
				return 0;
			}
			cached_iblkno = iblkno;
		}

		blkno = ilp[blkoff-(directlim+1)];
		return blkno;
	}

	/* Is it a double-indirect? */
	if (blkoff <= ind2lim) {
		/* Find the double-indirect block */
		diblkno = ip->i_block[EXT2_DIND_BLOCK];

		if (diblkno == 0) {
			return 0;
		}

		/* Read in the double-indirect block */
		if (cached_diblkno != diblkno) {
			offset = partition_offset + (long) diblkno * (long) ext2fs.blocksize;
			if (cons_read(dev, diblkbuf, ext2fs.blocksize, offset)
			    != ext2fs.blocksize)
			{
				printf("ext2_blkno: err reading dindr blk\n");
				return 0;
			}
			cached_diblkno = diblkno;
		}

		/* Find the single-indirect block pointer ... */
		iblkno = dlp[(blkoff - (ind1lim+1)) / ptrs_per_blk];

		if (iblkno == 0) {
			return 0;
		}

		/* Read the indirect block */
    
		if (cached_iblkno != iblkno) {
			offset = partition_offset + (long) iblkno * (long) ext2fs.blocksize;
			if (cons_read(dev, iblkbuf, ext2fs.blocksize, offset)
			    != ext2fs.blocksize)
			{
				printf("ext2_blkno: err on iblk read\n");
				return 0;
			}
			cached_iblkno = iblkno;
		}

		/* Find the block itself. */
		blkno = ilp[(blkoff-(ind1lim+1)) % ptrs_per_blk];
		return blkno;
	}

	if (blkoff > ind2lim) {
		printf("ext2_blkno: block number too large: %d\n", blkoff);
		return 0;
	}
	return -1;
}


static int ext2_breadi(struct ext2_inode *ip, long blkno, long nblks,
		       char *buffer)
{
	long dev_blkno, ncontig, offset, nbytes, tot_bytes;

	tot_bytes = 0;
	if ((blkno+nblks)*ext2fs.blocksize > ip->i_size)
		nblks = (ip->i_size + ext2fs.blocksize) / ext2fs.blocksize - blkno;

	while (nblks) {
		/*
		 * Contiguous reads are a lot faster, so we try to group
		 * as many blocks as possible:
		 */
		ncontig = 0; nbytes = 0;
		dev_blkno = ext2_blkno(ip, blkno);
		do {
			++blkno; ++ncontig; --nblks;
			nbytes += ext2fs.blocksize;
		} while (nblks &&
			 ext2_blkno(ip, blkno) == dev_blkno + ncontig);

		if (dev_blkno == 0) {
			/* This is a "hole" */
			memset(buffer, 0, nbytes);
		} else {
			/* Read it for real */
			offset = partition_offset + (long) dev_blkno* (long) ext2fs.blocksize;
#ifdef DEBUG_EXT2
			printf("ext2_bread: reading %ld bytes at offset %ld\n",
			       nbytes, offset);
#endif
			if (cons_read(dev, buffer, nbytes, offset)
			    != nbytes)
			{
				printf("ext2_bread: read error\n");
				return -1;
			}
		}
		buffer    += nbytes;
		tot_bytes += nbytes;
	}
	return tot_bytes;
}

static struct ext2_dir_entry_2 *ext2_readdiri(struct ext2_inode *dir_inode,
					      int rewind)
{
	struct ext2_dir_entry_2 *dp;
	static int diroffset = 0, blockoffset = 0;

	/* Reading a different directory, invalidate previous state */
	if (rewind) {
		diroffset = 0;
		blockoffset = 0;
		/* read first block */
		if (ext2_breadi(dir_inode, 0, 1, blkbuf) < 0)
			return NULL;
	}

#ifdef DEBUG_EXT2
	printf("ext2_readdiri: blkoffset %d diroffset %d len %d\n",
		blockoffset, diroffset, dir_inode->i_size);
#endif
	if (blockoffset >= ext2fs.blocksize) {
		diroffset += ext2fs.blocksize;
		if (diroffset >= dir_inode->i_size)
			return NULL;
#ifdef DEBUG_EXT2
		printf("ext2_readdiri: reading block at %d\n",
			diroffset);
#endif
		/* assume that this will read the whole block */
		if (ext2_breadi(dir_inode,
				diroffset / ext2fs.blocksize,
				1, blkbuf) < 0)
			return NULL;
		blockoffset = 0;
	}

	dp = (struct ext2_dir_entry_2 *) (blkbuf + blockoffset);
	blockoffset += dp->rec_len;
#ifdef DEBUG_EXT2
	printf("ext2_readdiri: returning %p = %.*s\n", dp, dp->name_len, dp->name);
#endif
	return dp;
}

static struct ext2_inode *ext2_namei(const char *name)
{
	char namebuf[256];
	char *component;
	struct ext2_inode *dir_inode;
	struct ext2_dir_entry_2 *dp;
	int next_ino;

	/* squirrel away a copy of "namebuf" that we can modify: */
	strcpy(namebuf, name);

	/* start at the root: */
	if (!root_inode)
		root_inode = ext2_iget(EXT2_ROOT_INO);
	dir_inode = root_inode;
	if (!dir_inode)
	  return NULL;

	component = strtok(namebuf, "/");
	while (component) {
		int component_length;
		int rewind = 0;
		/*
		 * Search for the specified component in the current
		 * directory inode.
		 */
		next_ino = -1;
		component_length = strlen(component);

		/* rewind the first time through */
		while ((dp = ext2_readdiri(dir_inode, !rewind++))) {
			if ((dp->name_len == component_length) &&
			    (strncmp(component, dp->name,
				     component_length) == 0))
			{
				/* Found it! */
#ifdef DEBUG_EXT2
				printf("ext2_namei: found entry %s\n",
					component);
#endif
				next_ino = dp->inode;
				break;
			}
#ifdef DEBUG_EXT2
			printf("ext2_namei: looping\n");
#endif
		}
	
#ifdef DEBUG_EXT2
		printf("ext2_namei: next_ino = %d\n", next_ino);
#endif

		/*
		 * At this point, we're done with this directory whether
		 * we've succeeded or failed...
		 */
		if (dir_inode != root_inode)
			ext2_iput(dir_inode);

		/*
		 * If next_ino is negative, then we've failed (gone
		 * all the way through without finding anything)
		 */
		if (next_ino < 0) {
			return NULL;
		}

		/*
		 * Otherwise, we can get this inode and find the next
		 * component string...
		 */
		dir_inode = ext2_iget(next_ino);
		if (!dir_inode)
		  return NULL;

		component = strtok(NULL, "/");
	}

	/*
	 * If we get here, then we got through all the components.
	 * Whatever we got must match up with the last one.
	 */
	return dir_inode;
}


/*
 * Read block number "blkno" from the specified file.
 */
static int ext2_bread(int fd, long blkno, long nblks, char *buffer)
{
	struct ext2_inode * ip;
	ip = &inode_table[fd].inode;
	return ext2_breadi(ip, blkno, nblks, buffer);
}

/*
 * Note: don't mix any kind of file lookup or other I/O with this or
 * you will lose horribly (as it reuses blkbuf)
 */
static const char * ext2_readdir(int fd, int rewind)
{
	struct ext2_inode * ip = &inode_table[fd].inode;
	struct ext2_dir_entry_2 * ent;
	if (!S_ISDIR(ip->i_mode)) {
		printf("fd %d (inode %d) is not a directory (mode %x)\n",
		       fd, inode_table[fd].inumber, ip->i_mode);
		return NULL;
	}
	ent = ext2_readdiri(ip, rewind);
	if (ent) {
		ent->name[ent->name_len] = '\0';
		return ent->name;
	} else { 
		return NULL;
	}
}

static int ext2_fstat(int fd, struct stat* buf)
{
	struct ext2_inode * ip = &inode_table[fd].inode;

	if (fd >= MAX_OPEN_FILES)
		return -1;
	memset(buf, 0, sizeof(struct stat));
	/* fill in relevant fields */
	buf->st_ino = inode_table[fd].inumber;
	buf->st_mode = ip->i_mode;
	buf->st_flags = ip->i_flags;
	buf->st_nlink = ip->i_links_count;
	buf->st_uid = ip->i_uid;
	buf->st_gid = ip->i_gid;
	buf->st_size = ip->i_size;
	buf->st_blocks = ip->i_blocks;
	buf->st_atime = ip->i_atime;
	buf->st_mtime = ip->i_mtime;
	buf->st_ctime = ip->i_ctime;

	return 0; /* NOTHING CAN GO WROGN! */
}

static struct ext2_inode * ext2_follow_link(struct ext2_inode * from,
					    const char * base)
{
	char *linkto;

	if (from->i_blocks) {
		linkto = blkbuf;
		if (ext2_breadi(from, 0, 1, blkbuf) == -1)
			return NULL;
#ifdef DEBUG_EXT2
		printf("long link!\n");
#endif
	} else {
		linkto = (char*)from->i_block;
	}
#ifdef DEBUG_EXT2
	printf("symlink to %s\n", linkto);
#endif

	/* Resolve relative links */
	if (linkto[0] != '/') {
		char *end = strrchr(base, '/');
		if (end) {
			char fullname[(end - base + 1) + strlen(linkto) + 1];
			strncpy(fullname, base, end - base + 1);
			fullname[end - base + 1] = '\0';
			strcat(fullname, linkto);
#ifdef DEBUG_EXT2
			printf("resolved to %s\n", fullname);
#endif
			return ext2_namei(fullname);
		} else {
			/* Assume it's in the root */
			return ext2_namei(linkto);
		}
	} else {
		return ext2_namei(linkto);
	}
}

static int ext2_open(const char *filename)
{
	/*
	 * Unix-like open routine.  Returns a small integer (actually
	 * an index into the inode table...
	 */
	struct ext2_inode * ip;

	ip = ext2_namei(filename);
	if (ip) {
		struct inode_table_entry *itp;

		while (S_ISLNK(ip->i_mode)) {
			ip = ext2_follow_link(ip, filename);
			if (!ip) return -1;
		}
		itp = (struct inode_table_entry *)ip;
		return itp - inode_table;
	} else
		return -1;
}


static void ext2_close(int fd)
{
	/* blah, hack, don't close the root inode ever */
	if (&inode_table[fd].inode != root_inode)
		ext2_iput(&inode_table[fd].inode);
}


struct bootfs ext2fs = {
	FS_EXT2, 0,
	ext2_mount,
	ext2_open,  ext2_bread,  ext2_close,
	ext2_readdir, ext2_fstat
};
