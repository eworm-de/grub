/*
 *  f2fs.c - Flash-Friendly File System
 *
 *  Written by Jaegeuk Kim <jaegeuk@kernel.org>
 *
 *  Copyright (C) 2015  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/err.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/dl.h>
#include <grub/types.h>
#include <grub/charset.h>
#include <grub/fshelp.h>
#include <grub/safemath.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* F2FS Magic Number. */
#define F2FS_SUPER_MAGIC          0xf2f52010

#define CHECKSUM_OFFSET           4092  /* Must be aligned 4 bytes. */
#define U32_CHECKSUM_OFFSET       (CHECKSUM_OFFSET >> 2)
#define CRCPOLY_LE                0xedb88320

/* Byte-size offset. */
#define F2FS_SUPER_OFFSET         ((grub_disk_addr_t)1024)
#define F2FS_SUPER_OFFSET0        (F2FS_SUPER_OFFSET >> GRUB_DISK_SECTOR_BITS)
#define F2FS_SUPER_OFFSET1        ((F2FS_SUPER_OFFSET + F2FS_BLKSIZE) >> \
                                        GRUB_DISK_SECTOR_BITS)

/* 9 bits for 512 bytes. */
#define F2FS_MIN_LOG_SECTOR_SIZE  9

/* Support only 4KB block. */
#define F2FS_BLK_BITS             12
#define F2FS_BLKSIZE              (1 << F2FS_BLK_BITS)
#define F2FS_BLK_SEC_BITS         (F2FS_BLK_BITS - GRUB_DISK_SECTOR_BITS)

#define VERSION_LEN               256
#define F2FS_MAX_EXTENSION        64

#define CP_COMPACT_SUM_FLAG       0x00000004
#define CP_UMOUNT_FLAG            0x00000001

#define MAX_ACTIVE_LOGS           16
#define MAX_ACTIVE_NODE_LOGS      8
#define MAX_ACTIVE_DATA_LOGS      8
#define NR_CURSEG_DATA_TYPE       3
#define NR_CURSEG_NODE_TYPE       3
#define NR_CURSEG_TYPE            (NR_CURSEG_DATA_TYPE + NR_CURSEG_NODE_TYPE)

#define ENTRIES_IN_SUM            512
#define SUMMARY_SIZE              7
#define SUM_FOOTER_SIZE           5
#define JENTRY_SIZE               (sizeof(struct grub_f2fs_nat_jent))
#define SUM_ENTRIES_SIZE          (SUMMARY_SIZE * ENTRIES_IN_SUM)
#define SUM_JOURNAL_SIZE          (F2FS_BLKSIZE - SUM_FOOTER_SIZE - SUM_ENTRIES_SIZE)
#define NAT_JOURNAL_ENTRIES       ((SUM_JOURNAL_SIZE - 2) / JENTRY_SIZE)
#define NAT_JOURNAL_RESERVED      ((SUM_JOURNAL_SIZE - 2) % JENTRY_SIZE)

#define NAT_ENTRY_SIZE            (sizeof(struct grub_f2fs_nat_entry))
#define NAT_ENTRY_PER_BLOCK       (F2FS_BLKSIZE / NAT_ENTRY_SIZE)

#define F2FS_NAME_LEN             255
#define F2FS_SLOT_LEN             8
#define NR_DENTRY_IN_BLOCK        214
#define SIZE_OF_DIR_ENTRY         11    /* By byte. */
#define BITS_PER_BYTE             8
#define SIZE_OF_DENTRY_BITMAP     ((NR_DENTRY_IN_BLOCK + BITS_PER_BYTE - 1) / \
                                        BITS_PER_BYTE)
#define SIZE_OF_RESERVED          (F2FS_BLKSIZE - \
                                        ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
                                        NR_DENTRY_IN_BLOCK + SIZE_OF_DENTRY_BITMAP))

#define F2FS_INLINE_XATTR_ADDRS   50    /* 200 bytes for inline xattrs. */
#define DEF_ADDRS_PER_INODE       923   /* Address Pointers in an Inode. */

#define ADDRS_PER_BLOCK           1018  /* Address Pointers in a Direct Block. */
#define NIDS_PER_BLOCK            1018  /* Node IDs in an Indirect Block. */
#define NODE_DIR1_BLOCK           (DEF_ADDRS_PER_INODE + 1)
#define NODE_DIR2_BLOCK           (DEF_ADDRS_PER_INODE + 2)
#define NODE_IND1_BLOCK           (DEF_ADDRS_PER_INODE + 3)
#define NODE_IND2_BLOCK           (DEF_ADDRS_PER_INODE + 4)
#define NODE_DIND_BLOCK           (DEF_ADDRS_PER_INODE + 5)

#define MAX_INLINE_DATA           (4 * (DEF_ADDRS_PER_INODE - \
                                        F2FS_INLINE_XATTR_ADDRS - 1))
#define NR_INLINE_DENTRY          (MAX_INLINE_DATA * BITS_PER_BYTE / \
                                        ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
                                        BITS_PER_BYTE + 1))
#define INLINE_DENTRY_BITMAP_SIZE ((NR_INLINE_DENTRY + BITS_PER_BYTE - 1) / \
                                        BITS_PER_BYTE)
#define INLINE_RESERVED_SIZE      (MAX_INLINE_DATA - \
                                        ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
                                        NR_INLINE_DENTRY + \
                                        INLINE_DENTRY_BITMAP_SIZE))
#define CURSEG_HOT_DATA           0

#define CKPT_FLAG_SET(ckpt, f)    (ckpt)->ckpt_flags & \
                                        grub_cpu_to_le32_compile_time (f)

#define F2FS_INLINE_XATTR         0x01  /* File inline xattr flag. */
#define F2FS_INLINE_DATA          0x02  /* File inline data flag. */
#define F2FS_INLINE_DENTRY        0x04  /* File inline dentry flag. */
#define F2FS_DATA_EXIST           0x08  /* File inline data exist flag. */
#define F2FS_INLINE_DOTS          0x10  /* File having implicit dot dentries. */

#define MAX_VOLUME_NAME           512
#define MAX_NAT_BITMAP_SIZE       3900

enum FILE_TYPE
{
  F2FS_FT_UNKNOWN,
  F2FS_FT_REG_FILE                = 1,
  F2FS_FT_DIR                     = 2,
  F2FS_FT_SYMLINK                 = 7
};

struct grub_f2fs_superblock
{
  grub_uint32_t                   magic;
  grub_uint16_t                   dummy1[2];
  grub_uint32_t                   log_sectorsize;
  grub_uint32_t                   log_sectors_per_block;
  grub_uint32_t                   log_blocksize;
  grub_uint32_t                   log_blocks_per_seg;
  grub_uint32_t                   segs_per_sec;
  grub_uint32_t                   secs_per_zone;
  grub_uint32_t                   checksum_offset;
  grub_uint8_t                    dummy2[40];
  grub_uint32_t                   cp_blkaddr;
  grub_uint32_t                   sit_blkaddr;
  grub_uint32_t                   nat_blkaddr;
  grub_uint32_t                   ssa_blkaddr;
  grub_uint32_t                   main_blkaddr;
  grub_uint32_t                   root_ino;
  grub_uint32_t                   node_ino;
  grub_uint32_t                   meta_ino;
  grub_uint8_t                    uuid[16];
  grub_uint16_t                   volume_name[MAX_VOLUME_NAME];
  grub_uint32_t                   extension_count;
  grub_uint8_t                    extension_list[F2FS_MAX_EXTENSION][8];
  grub_uint32_t                   cp_payload;
  grub_uint8_t                    version[VERSION_LEN];
  grub_uint8_t                    init_version[VERSION_LEN];
} GRUB_PACKED;

struct grub_f2fs_checkpoint
{
  grub_uint64_t                   checkpoint_ver;
  grub_uint64_t                   user_block_count;
  grub_uint64_t                   valid_block_count;
  grub_uint32_t                   rsvd_segment_count;
  grub_uint32_t                   overprov_segment_count;
  grub_uint32_t                   free_segment_count;
  grub_uint32_t                   cur_node_segno[MAX_ACTIVE_NODE_LOGS];
  grub_uint16_t                   cur_node_blkoff[MAX_ACTIVE_NODE_LOGS];
  grub_uint32_t                   cur_data_segno[MAX_ACTIVE_DATA_LOGS];
  grub_uint16_t                   cur_data_blkoff[MAX_ACTIVE_DATA_LOGS];
  grub_uint32_t                   ckpt_flags;
  grub_uint32_t                   cp_pack_total_block_count;
  grub_uint32_t                   cp_pack_start_sum;
  grub_uint32_t                   valid_node_count;
  grub_uint32_t                   valid_inode_count;
  grub_uint32_t                   next_free_nid;
  grub_uint32_t                   sit_ver_bitmap_bytesize;
  grub_uint32_t                   nat_ver_bitmap_bytesize;
  grub_uint32_t                   checksum_offset;
  grub_uint64_t                   elapsed_time;
  grub_uint8_t                    alloc_type[MAX_ACTIVE_LOGS];
  grub_uint8_t                    sit_nat_version_bitmap[MAX_NAT_BITMAP_SIZE];
  grub_uint32_t                   checksum;
} GRUB_PACKED;

struct grub_f2fs_nat_entry {
  grub_uint8_t                    version;
  grub_uint32_t                   ino;
  grub_uint32_t                   block_addr;
} GRUB_PACKED;

struct grub_f2fs_nat_jent
{
  grub_uint32_t                   nid;
  struct grub_f2fs_nat_entry      ne;
} GRUB_PACKED;

struct grub_f2fs_nat_journal {
  grub_uint16_t                   n_nats;
  struct grub_f2fs_nat_jent       entries[NAT_JOURNAL_ENTRIES];
  grub_uint8_t                    reserved[NAT_JOURNAL_RESERVED];
} GRUB_PACKED;

struct grub_f2fs_nat_block {
  struct grub_f2fs_nat_entry      ne[NAT_ENTRY_PER_BLOCK];
} GRUB_PACKED;

struct grub_f2fs_dir_entry
{
  grub_uint32_t                   hash_code;
  grub_uint32_t                   ino;
  grub_uint16_t                   name_len;
  grub_uint8_t                    file_type;
} GRUB_PACKED;

struct grub_f2fs_inline_dentry
{
  grub_uint8_t                    dentry_bitmap[INLINE_DENTRY_BITMAP_SIZE];
  grub_uint8_t                    reserved[INLINE_RESERVED_SIZE];
  struct grub_f2fs_dir_entry      dentry[NR_INLINE_DENTRY];
  grub_uint8_t                    filename[NR_INLINE_DENTRY][F2FS_SLOT_LEN];
} GRUB_PACKED;

struct grub_f2fs_dentry_block {
  grub_uint8_t                    dentry_bitmap[SIZE_OF_DENTRY_BITMAP];
  grub_uint8_t                    reserved[SIZE_OF_RESERVED];
  struct grub_f2fs_dir_entry      dentry[NR_DENTRY_IN_BLOCK];
  grub_uint8_t                    filename[NR_DENTRY_IN_BLOCK][F2FS_SLOT_LEN];
} GRUB_PACKED;

struct grub_f2fs_inode
{
  grub_uint16_t                   i_mode;
  grub_uint8_t                    i_advise;
  grub_uint8_t                    i_inline;
  grub_uint32_t                   i_uid;
  grub_uint32_t                   i_gid;
  grub_uint32_t                   i_links;
  grub_uint64_t                   i_size;
  grub_uint64_t                   i_blocks;
  grub_uint64_t                   i_atime;
  grub_uint64_t                   i_ctime;
  grub_uint64_t                   i_mtime;
  grub_uint32_t                   i_atime_nsec;
  grub_uint32_t                   i_ctime_nsec;
  grub_uint32_t                   i_mtime_nsec;
  grub_uint32_t                   i_generation;
  grub_uint32_t                   i_current_depth;
  grub_uint32_t                   i_xattr_nid;
  grub_uint32_t                   i_flags;
  grub_uint32_t                   i_pino;
  grub_uint32_t                   i_namelen;
  grub_uint8_t                    i_name[F2FS_NAME_LEN];
  grub_uint8_t                    i_dir_level;
  grub_uint8_t                    i_ext[12];
  grub_uint32_t                   i_addr[DEF_ADDRS_PER_INODE];
  grub_uint32_t                   i_nid[5];
} GRUB_PACKED;

struct grub_direct_node {
  grub_uint32_t                   addr[ADDRS_PER_BLOCK];
} GRUB_PACKED;

struct grub_indirect_node {
  grub_uint32_t                   nid[NIDS_PER_BLOCK];
} GRUB_PACKED;

struct grub_f2fs_node
{
  union
  {
    struct grub_f2fs_inode        i;
    struct grub_direct_node       dn;
    struct grub_indirect_node     in;
    /* Should occupy F2FS_BLKSIZE totally. */
    char                          buf[F2FS_BLKSIZE - 40];
  };
  grub_uint8_t                    dummy[40];
} GRUB_PACKED;

struct grub_fshelp_node
{
  struct grub_f2fs_data           *data;
  struct grub_f2fs_node           inode;
  grub_uint32_t                   ino;
  int inode_read;
};

struct grub_f2fs_data
{
  struct grub_f2fs_superblock     sblock;
  struct grub_f2fs_checkpoint     ckpt;

  grub_uint32_t                   root_ino;
  grub_uint32_t                   blocks_per_seg;
  grub_uint32_t                   cp_blkaddr;
  grub_uint32_t                   nat_blkaddr;

  struct grub_f2fs_nat_journal    nat_j;
  char                            *nat_bitmap;
  grub_uint32_t                   nat_bitmap_size;

  grub_disk_t                     disk;
  struct grub_f2fs_node           *inode;
  struct grub_fshelp_node         diropen;
};

struct grub_f2fs_dir_iter_ctx
{
  struct grub_f2fs_data           *data;
  grub_fshelp_iterate_dir_hook_t  hook;
  void                            *hook_data;
  grub_uint8_t                    *bitmap;
  grub_uint8_t                    (*filename)[F2FS_SLOT_LEN];
  struct grub_f2fs_dir_entry      *dentry;
  int                             max;
};

struct grub_f2fs_dir_ctx
{
  grub_fs_dir_hook_t              hook;
  void                            *hook_data;
  struct grub_f2fs_data           *data;
};

static grub_dl_t my_mod;

static int
grub_f2fs_test_bit_le (int nr, const grub_uint8_t *addr)
{
  return addr[nr >> 3] & (1 << (nr & 7));
}

static char *
get_inline_addr (struct grub_f2fs_inode *inode)
{
  return (char *) &inode->i_addr[1];
}

static grub_uint64_t
grub_f2fs_file_size (struct grub_f2fs_inode *inode)
{
  return grub_le_to_cpu64 (inode->i_size);
}

static grub_uint32_t
start_cp_addr (struct grub_f2fs_data *data)
{
  struct grub_f2fs_checkpoint *ckpt = &data->ckpt;
  grub_uint32_t start_addr = data->cp_blkaddr;

  if (!(ckpt->checkpoint_ver & grub_cpu_to_le64_compile_time(1)))
    return start_addr + data->blocks_per_seg;

  return start_addr;
}

static grub_uint32_t
start_sum_block (struct grub_f2fs_data *data)
{
  struct grub_f2fs_checkpoint *ckpt = &data->ckpt;

  return start_cp_addr (data) + grub_le_to_cpu32 (ckpt->cp_pack_start_sum);
}

static grub_uint32_t
sum_blk_addr (struct grub_f2fs_data *data, int base, int type)
{
  struct grub_f2fs_checkpoint *ckpt = &data->ckpt;

  return start_cp_addr (data) +
           grub_le_to_cpu32 (ckpt->cp_pack_total_block_count) -
           (base + 1) + type;
}

static void *
nat_bitmap_ptr (struct grub_f2fs_data *data, grub_uint32_t *nat_bitmap_size)
{
  struct grub_f2fs_checkpoint *ckpt = &data->ckpt;
  grub_uint32_t offset;
  *nat_bitmap_size = MAX_NAT_BITMAP_SIZE;

  if (grub_le_to_cpu32 (data->sblock.cp_payload) > 0)
    return ckpt->sit_nat_version_bitmap;

  offset = grub_le_to_cpu32 (ckpt->sit_ver_bitmap_bytesize);
  if (offset >= MAX_NAT_BITMAP_SIZE)
     return NULL;

  *nat_bitmap_size = *nat_bitmap_size - offset;

  return ckpt->sit_nat_version_bitmap + offset;
}

static grub_uint32_t
get_node_id (struct grub_f2fs_node *rn, int off, int inode_block)
{
  if (inode_block)
    return grub_le_to_cpu32 (rn->i.i_nid[off - NODE_DIR1_BLOCK]);

  return grub_le_to_cpu32 (rn->in.nid[off]);
}

static grub_err_t
grub_f2fs_block_read (struct grub_f2fs_data *data, grub_uint32_t blkaddr,
                      void *buf)
{
  return grub_disk_read (data->disk,
                         ((grub_disk_addr_t)blkaddr) << F2FS_BLK_SEC_BITS,
                         0, F2FS_BLKSIZE, buf);
}

/* CRC32 */
static grub_uint32_t
grub_f2fs_cal_crc32 (const void *buf, const grub_uint32_t len)
{
  grub_uint32_t crc = F2FS_SUPER_MAGIC;
  unsigned char *p = (unsigned char *)buf;
  grub_uint32_t tmp = len;
  int i;

  while (tmp--)
    {
      crc ^= *p++;
      for (i = 0; i < 8; i++)
        crc = (crc >> 1) ^ ((crc & 1) ? CRCPOLY_LE : 0);
    }

  return crc;
}

static int
grub_f2fs_crc_valid (grub_uint32_t blk_crc, void *buf, const grub_uint32_t len)
{
  grub_uint32_t cal_crc = 0;

  cal_crc = grub_f2fs_cal_crc32 (buf, len);

  return (cal_crc == blk_crc) ? 1 : 0;
}

static int
grub_f2fs_test_bit (grub_uint32_t nr, const char *p, grub_uint32_t len)
{
  int mask;
  grub_uint32_t shifted_nr = (nr >> 3);

  if (shifted_nr >= len)
    return -1;

  p += shifted_nr;
  mask = 1 << (7 - (nr & 0x07));

  return mask & *p;
}

static int
grub_f2fs_sanity_check_sb (struct grub_f2fs_superblock *sb)
{
  grub_uint32_t log_sectorsize, log_sectors_per_block;

  if (sb->magic != grub_cpu_to_le32_compile_time (F2FS_SUPER_MAGIC))
    return -1;

  if (sb->log_blocksize != grub_cpu_to_le32_compile_time (F2FS_BLK_BITS))
    return -1;

  log_sectorsize = grub_le_to_cpu32 (sb->log_sectorsize);
  log_sectors_per_block = grub_le_to_cpu32 (sb->log_sectors_per_block);

  if (log_sectorsize > F2FS_BLK_BITS)
    return -1;

  if (log_sectorsize < F2FS_MIN_LOG_SECTOR_SIZE)
    return -1;

  if (log_sectors_per_block + log_sectorsize != F2FS_BLK_BITS)
    return -1;

  return 0;
}

static int
grub_f2fs_read_sb (struct grub_f2fs_data *data, grub_disk_addr_t offset)
{
  grub_disk_t disk = data->disk;
  grub_err_t err;

  /* Read first super block. */
  err = grub_disk_read (disk, offset, 0, sizeof (data->sblock), &data->sblock);
  if (err)
    return -1;

  return grub_f2fs_sanity_check_sb (&data->sblock);
}

static void *
validate_checkpoint (struct grub_f2fs_data *data, grub_uint32_t cp_addr,
                     grub_uint64_t *version)
{
  grub_uint32_t *cp_page_1, *cp_page_2;
  struct grub_f2fs_checkpoint *cp_block;
  grub_uint64_t cur_version = 0, pre_version = 0;
  grub_uint32_t crc = 0;
  grub_uint32_t crc_offset;
  grub_err_t err;

  /* Read the 1st cp block in this CP pack. */
  cp_page_1 = grub_malloc (F2FS_BLKSIZE);
  if (!cp_page_1)
    return NULL;

  err = grub_f2fs_block_read (data, cp_addr, cp_page_1);
  if (err)
    goto invalid_cp1;

  cp_block = (struct grub_f2fs_checkpoint *)cp_page_1;
  crc_offset = grub_le_to_cpu32 (cp_block->checksum_offset);
  if (crc_offset != CHECKSUM_OFFSET)
    goto invalid_cp1;

  crc = grub_le_to_cpu32 (*(cp_page_1 + U32_CHECKSUM_OFFSET));
  if (!grub_f2fs_crc_valid (crc, cp_block, crc_offset))
    goto invalid_cp1;

  pre_version = grub_le_to_cpu64 (cp_block->checkpoint_ver);

  /* Read the 2nd cp block in this CP pack. */
  cp_page_2 = grub_malloc (F2FS_BLKSIZE);
  if (!cp_page_2)
    goto invalid_cp1;

  cp_addr += grub_le_to_cpu32 (cp_block->cp_pack_total_block_count) - 1;

  err = grub_f2fs_block_read (data, cp_addr, cp_page_2);
  if (err)
    goto invalid_cp2;

  cp_block = (struct grub_f2fs_checkpoint *)cp_page_2;
  crc_offset = grub_le_to_cpu32 (cp_block->checksum_offset);
  if (crc_offset != CHECKSUM_OFFSET)
    goto invalid_cp2;

  crc = grub_le_to_cpu32 (*(cp_page_2 + U32_CHECKSUM_OFFSET));
  if (!grub_f2fs_crc_valid (crc, cp_block, crc_offset))
    goto invalid_cp2;

  cur_version = grub_le_to_cpu64 (cp_block->checkpoint_ver);
  if (cur_version == pre_version)
    {
      *version = cur_version;
      grub_free (cp_page_2);

      return cp_page_1;
    }

 invalid_cp2:
  grub_free (cp_page_2);

 invalid_cp1:
  grub_free (cp_page_1);

  return NULL;
}

static grub_err_t
grub_f2fs_read_cp (struct grub_f2fs_data *data)
{
  void *cp1, *cp2, *cur_page;
  grub_uint64_t cp1_version = 0, cp2_version = 0;
  grub_uint64_t cp_start_blk_no;

  /*
   * Finding out valid cp block involves read both
   * sets (cp pack1 and cp pack 2).
   */
  cp_start_blk_no = data->cp_blkaddr;
  cp1 = validate_checkpoint (data, cp_start_blk_no, &cp1_version);
  if (!cp1 && grub_errno)
    return grub_errno;

  /* The second checkpoint pack should start at the next segment. */
  cp_start_blk_no += data->blocks_per_seg;
  cp2 = validate_checkpoint (data, cp_start_blk_no, &cp2_version);
  if (!cp2 && grub_errno)
    {
      grub_free (cp1);
      return grub_errno;
    }

  if (cp1 && cp2)
    cur_page = (cp2_version > cp1_version) ? cp2 : cp1;
  else if (cp1)
    cur_page = cp1;
  else if (cp2)
    cur_page = cp2;
  else
    return grub_error (GRUB_ERR_BAD_FS, "no checkpoints");

  grub_memcpy (&data->ckpt, cur_page, F2FS_BLKSIZE);

  grub_free (cp1);
  grub_free (cp2);

  return 0;
}

static grub_err_t
get_nat_journal (struct grub_f2fs_data *data)
{
  grub_uint32_t block;
  char *buf;
  grub_err_t err;

  buf = grub_malloc (F2FS_BLKSIZE);
  if (!buf)
    return grub_errno;

  if (CKPT_FLAG_SET(&data->ckpt, CP_COMPACT_SUM_FLAG))
    block = start_sum_block (data);
  else if (CKPT_FLAG_SET (&data->ckpt, CP_UMOUNT_FLAG))
    block = sum_blk_addr (data, NR_CURSEG_TYPE, CURSEG_HOT_DATA);
  else
    block = sum_blk_addr (data, NR_CURSEG_DATA_TYPE, CURSEG_HOT_DATA);

  err = grub_f2fs_block_read (data, block, buf);
  if (err)
    goto fail;

  if (CKPT_FLAG_SET (&data->ckpt, CP_COMPACT_SUM_FLAG))
    grub_memcpy (&data->nat_j, buf, SUM_JOURNAL_SIZE);
  else
    grub_memcpy (&data->nat_j, buf + SUM_ENTRIES_SIZE, SUM_JOURNAL_SIZE);

 fail:
  grub_free (buf);

  return err;
}

static grub_err_t
get_blkaddr_from_nat_journal (struct grub_f2fs_data *data, grub_uint32_t nid,
                              grub_uint32_t *blkaddr)
{
  grub_uint16_t n = grub_le_to_cpu16 (data->nat_j.n_nats);
  grub_uint16_t i;

  if (n > NAT_JOURNAL_ENTRIES)
    return grub_error (GRUB_ERR_BAD_FS,
                       "invalid number of nat journal entries");

  for (i = 0; i < n; i++)
    {
      if (grub_le_to_cpu32 (data->nat_j.entries[i].nid) == nid)
        {
          *blkaddr = grub_le_to_cpu32 (data->nat_j.entries[i].ne.block_addr);
          break;
        }
    }

  return GRUB_ERR_NONE;
}

static grub_uint32_t
get_node_blkaddr (struct grub_f2fs_data *data, grub_uint32_t nid)
{
  struct grub_f2fs_nat_block *nat_block;
  grub_uint32_t seg_off, block_off, entry_off, block_addr;
  grub_uint32_t blkaddr = 0;
  grub_err_t err;
  int result_bit;

  err = get_blkaddr_from_nat_journal (data, nid, &blkaddr);
  if (err != GRUB_ERR_NONE)
    return 0;

  if (blkaddr)
    return blkaddr;

  nat_block = grub_malloc (F2FS_BLKSIZE);
  if (!nat_block)
    return 0;

  block_off = nid / NAT_ENTRY_PER_BLOCK;
  entry_off = nid % NAT_ENTRY_PER_BLOCK;

  seg_off = block_off / data->blocks_per_seg;
  block_addr = data->nat_blkaddr +
        ((seg_off * data->blocks_per_seg) << 1) +
        (block_off & (data->blocks_per_seg - 1));

  result_bit = grub_f2fs_test_bit (block_off, data->nat_bitmap,
                                   data->nat_bitmap_size);
  if (result_bit > 0)
    block_addr += data->blocks_per_seg;
  else if (result_bit == -1)
    {
      grub_free (nat_block);
      return 0;
    }

  err = grub_f2fs_block_read (data, block_addr, nat_block);
  if (err)
    {
      grub_free (nat_block);
      return 0;
    }

  blkaddr = grub_le_to_cpu32 (nat_block->ne[entry_off].block_addr);

  grub_free (nat_block);

  return blkaddr;
}

static int
grub_get_node_path (struct grub_f2fs_inode *inode, grub_uint32_t block,
                    grub_uint32_t offset[4], grub_uint32_t noffset[4])
{
  grub_uint32_t direct_blks = ADDRS_PER_BLOCK;
  grub_uint32_t dptrs_per_blk = NIDS_PER_BLOCK;
  grub_uint32_t indirect_blks = ADDRS_PER_BLOCK * NIDS_PER_BLOCK;
  grub_uint32_t dindirect_blks = indirect_blks * NIDS_PER_BLOCK;
  grub_uint32_t direct_index = DEF_ADDRS_PER_INODE;
  int n = 0;
  int level = -1;

  if (inode->i_inline & F2FS_INLINE_XATTR)
    direct_index -= F2FS_INLINE_XATTR_ADDRS;

  noffset[0] = 0;

  if (block < direct_index)
    {
      offset[n] = block;
      level = 0;
      goto got;
    }

  block -= direct_index;
  if (block < direct_blks)
    {
      offset[n++] = NODE_DIR1_BLOCK;
      noffset[n] = 1;
      offset[n] = block;
      level = 1;
      goto got;
    }

  block -= direct_blks;
  if (block < direct_blks)
    {
      offset[n++] = NODE_DIR2_BLOCK;
      noffset[n] = 2;
      offset[n] = block;
      level = 1;
      goto got;
    }

  block -= direct_blks;
  if (block < indirect_blks)
    {
      offset[n++] = NODE_IND1_BLOCK;
      noffset[n] = 3;
      offset[n++] = block / direct_blks;
      noffset[n] = 4 + offset[n - 1];
      offset[n] = block % direct_blks;
      level = 2;
      goto got;
    }

  block -= indirect_blks;
  if (block < indirect_blks)
    {
      offset[n++] = NODE_IND2_BLOCK;
      noffset[n] = 4 + dptrs_per_blk;
      offset[n++] = block / direct_blks;
      noffset[n] = 5 + dptrs_per_blk + offset[n - 1];
      offset[n] = block % direct_blks;
      level = 2;
      goto got;
    }

  block -= indirect_blks;
  if (block < dindirect_blks)
    {
      offset[n++] = NODE_DIND_BLOCK;
      noffset[n] = 5 + (dptrs_per_blk * 2);
      offset[n++] = block / indirect_blks;
      noffset[n] = 6 + (dptrs_per_blk * 2) +
      offset[n - 1] * (dptrs_per_blk + 1);
      offset[n++] = (block / direct_blks) % dptrs_per_blk;
      noffset[n] = 7 + (dptrs_per_blk * 2) +
      offset[n - 2] * (dptrs_per_blk + 1) + offset[n - 1];
      offset[n] = block % direct_blks;
      level = 3;
      goto got;
    }

 got:
  return level;
}

static grub_err_t
grub_f2fs_read_node (struct grub_f2fs_data *data,
                     grub_uint32_t nid, struct grub_f2fs_node *np)
{
  grub_uint32_t blkaddr;

  blkaddr = get_node_blkaddr (data, nid);
  if (!blkaddr)
    return grub_errno;

  return grub_f2fs_block_read (data, blkaddr, np);
}

static struct grub_f2fs_data *
grub_f2fs_mount (grub_disk_t disk)
{
  struct grub_f2fs_data *data;
  grub_err_t err;

  data = grub_malloc (sizeof (*data));
  if (!data)
    return NULL;

  data->disk = disk;

  if (grub_f2fs_read_sb (data, F2FS_SUPER_OFFSET0))
    {
      if (grub_f2fs_read_sb (data, F2FS_SUPER_OFFSET1))
        {
          if (grub_errno == GRUB_ERR_NONE)
            grub_error (GRUB_ERR_BAD_FS,
                        "not a F2FS filesystem (no superblock)");
          goto fail;
        }
    }

  data->root_ino = grub_le_to_cpu32 (data->sblock.root_ino);
  data->cp_blkaddr = grub_le_to_cpu32 (data->sblock.cp_blkaddr);
  data->nat_blkaddr = grub_le_to_cpu32 (data->sblock.nat_blkaddr);
  data->blocks_per_seg = 1 <<
    grub_le_to_cpu32 (data->sblock.log_blocks_per_seg);

  err = grub_f2fs_read_cp (data);
  if (err)
    goto fail;

  data->nat_bitmap = nat_bitmap_ptr (data, &data->nat_bitmap_size);
  if (data->nat_bitmap == NULL)
    goto fail;

  err = get_nat_journal (data);
  if (err)
    goto fail;

  data->diropen.data = data;
  data->diropen.ino = data->root_ino;
  data->diropen.inode_read = 1;
  data->inode = &data->diropen.inode;

  err = grub_f2fs_read_node (data, data->root_ino, data->inode);
  if (err)
    goto fail;

  return data;

 fail:
  if (grub_errno == GRUB_ERR_NONE)
    grub_error (GRUB_ERR_BAD_FS, "not a F2FS filesystem");

  grub_free (data);

  return NULL;
}

/* Guarantee inline_data was handled by caller. */
static grub_disk_addr_t
grub_f2fs_get_block (grub_fshelp_node_t node, grub_disk_addr_t block_ofs)
{
  struct grub_f2fs_data *data = node->data;
  struct grub_f2fs_inode *inode = &node->inode.i;
  grub_uint32_t offset[4], noffset[4], nids[4];
  struct grub_f2fs_node *node_block;
  grub_uint32_t block_addr = -1;
  int level, i;

  level = grub_get_node_path (inode, block_ofs, offset, noffset);

  if (level < 0)
    return -1;

  if (level == 0)
    return grub_le_to_cpu32 (inode->i_addr[offset[0]]);

  node_block = grub_malloc (F2FS_BLKSIZE);
  if (!node_block)
    return -1;

  nids[1] = get_node_id (&node->inode, offset[0], 1);

  /* Get indirect or direct nodes. */
  for (i = 1; i <= level; i++)
    {
      grub_f2fs_read_node (data, nids[i], node_block);
      if (grub_errno)
        goto fail;

      if (i < level)
        nids[i + 1] = get_node_id (node_block, offset[i], 0);
    }

  block_addr = grub_le_to_cpu32 (node_block->dn.addr[offset[level]]);

 fail:
  grub_free (node_block);

  return block_addr;
}

static grub_ssize_t
grub_f2fs_read_file (grub_fshelp_node_t node,
                     grub_disk_read_hook_t read_hook, void *read_hook_data,
                     grub_off_t pos, grub_size_t len, char *buf)
{
  struct grub_f2fs_inode *inode = &node->inode.i;
  grub_off_t filesize = grub_f2fs_file_size (inode);
  char *inline_addr = get_inline_addr (inode);

  if (inode->i_inline & F2FS_INLINE_DATA)
    {
      if (filesize > MAX_INLINE_DATA)
        return -1;

      if (len > filesize - pos)
        len = filesize - pos;

      grub_memcpy (buf, inline_addr + pos, len);
      return len;
    }

  return grub_fshelp_read_file (node->data->disk, node,
                                read_hook, read_hook_data,
                                pos, len, buf, grub_f2fs_get_block,
                                filesize,
                                F2FS_BLK_SEC_BITS, 0);
}

static char *
grub_f2fs_read_symlink (grub_fshelp_node_t node)
{
  char *symlink;
  struct grub_fshelp_node *diro = node;
  grub_uint64_t filesize;
  grub_size_t sz;

  if (!diro->inode_read)
    {
      grub_f2fs_read_node (diro->data, diro->ino, &diro->inode);
      if (grub_errno)
        return 0;
    }

  filesize = grub_f2fs_file_size(&diro->inode.i);

  if (grub_add (filesize, 1, &sz))
    {
      grub_error (GRUB_ERR_OUT_OF_RANGE, N_("symlink size overflow"));
      return 0;
    }
  symlink = grub_malloc (sz);
  if (!symlink)
    return 0;

  grub_f2fs_read_file (diro, 0, 0, 0, filesize, symlink);
  if (grub_errno)
    {
      grub_free (symlink);
      return 0;
    }

  symlink[filesize] = '\0';

  return symlink;
}

static int
grub_f2fs_check_dentries (struct grub_f2fs_dir_iter_ctx *ctx)
{
  struct grub_fshelp_node *fdiro;
  int i;

  for (i = 0; i < ctx->max;)
    {
      char *filename;
      enum grub_fshelp_filetype type = GRUB_FSHELP_UNKNOWN;
      enum FILE_TYPE ftype;
      int name_len;
      int ret;
      int sz;

      if (grub_f2fs_test_bit_le (i, ctx->bitmap) == 0)
        {
          i++;
          continue;
        }

      ftype = ctx->dentry[i].file_type;
      name_len = grub_le_to_cpu16 (ctx->dentry[i].name_len);

      if (name_len >= F2FS_NAME_LEN)
        return 0;

      if (grub_add (name_len, 1, &sz))
	{
	  grub_error (GRUB_ERR_OUT_OF_RANGE, N_("directory entry name length overflow"));
	  return 0;
	}
      filename = grub_malloc (sz);
      if (!filename)
        return 0;

      grub_memcpy (filename, ctx->filename[i], name_len);
      filename[name_len] = 0;

      fdiro = grub_malloc (sizeof (struct grub_fshelp_node));
      if (!fdiro)
        {
          grub_free(filename);
          return 0;
        }

      if (ftype == F2FS_FT_DIR)
        type = GRUB_FSHELP_DIR;
      else if (ftype == F2FS_FT_SYMLINK)
        type = GRUB_FSHELP_SYMLINK;
      else if (ftype == F2FS_FT_REG_FILE)
        type = GRUB_FSHELP_REG;

      fdiro->data = ctx->data;
      fdiro->ino = grub_le_to_cpu32 (ctx->dentry[i].ino);
      fdiro->inode_read = 0;

      ret = ctx->hook (filename, type, fdiro, ctx->hook_data);
      grub_free(filename);
      if (ret)
        return 1;

      i += (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;
    }

    return 0;
}

static int
grub_f2fs_iterate_inline_dir (struct grub_f2fs_inode *dir,
                              struct grub_f2fs_dir_iter_ctx *ctx)
{
  struct grub_f2fs_inline_dentry *de_blk;

  de_blk = (struct grub_f2fs_inline_dentry *) get_inline_addr (dir);

  ctx->bitmap = de_blk->dentry_bitmap;
  ctx->dentry = de_blk->dentry;
  ctx->filename = de_blk->filename;
  ctx->max = NR_INLINE_DENTRY;

  return grub_f2fs_check_dentries (ctx);
}

static int
grub_f2fs_iterate_dir (grub_fshelp_node_t dir,
                       grub_fshelp_iterate_dir_hook_t hook, void *hook_data)
{
  struct grub_fshelp_node *diro = (struct grub_fshelp_node *) dir;
  struct grub_f2fs_inode *inode;
  struct grub_f2fs_dir_iter_ctx ctx = {
    .data = diro->data,
    .hook = hook,
    .hook_data = hook_data
  };
  grub_off_t fpos = 0;

  if (!diro->inode_read)
    {
      grub_f2fs_read_node (diro->data, diro->ino, &diro->inode);
      if (grub_errno)
        return 0;
    }

  inode = &diro->inode.i;

  if (inode->i_inline & F2FS_INLINE_DENTRY)
    return grub_f2fs_iterate_inline_dir (inode, &ctx);

  while (fpos < grub_f2fs_file_size (inode))
    {
      struct grub_f2fs_dentry_block *de_blk;
      char *buf;
      int ret;

      buf = grub_zalloc (F2FS_BLKSIZE);
      if (!buf)
        return 0;

      grub_f2fs_read_file (diro, 0, 0, fpos, F2FS_BLKSIZE, buf);
      if (grub_errno)
        {
          grub_free (buf);
          return 0;
        }

      de_blk = (struct grub_f2fs_dentry_block *) buf;

      ctx.bitmap = de_blk->dentry_bitmap;
      ctx.dentry = de_blk->dentry;
      ctx.filename = de_blk->filename;
      ctx.max = NR_DENTRY_IN_BLOCK;

      ret = grub_f2fs_check_dentries (&ctx);
      grub_free (buf);
      if (ret)
        return 1;

      fpos += F2FS_BLKSIZE;
    }

  return 0;
}

static int
grub_f2fs_dir_iter (const char *filename, enum grub_fshelp_filetype filetype,
                    grub_fshelp_node_t node, void *data)
{
  struct grub_f2fs_dir_ctx *ctx = data;
  struct grub_dirhook_info info;

  grub_memset (&info, 0, sizeof (info));
  if (!node->inode_read)
    {
      grub_f2fs_read_node (ctx->data, node->ino, &node->inode);
      if (!grub_errno)
        node->inode_read = 1;
      grub_errno = GRUB_ERR_NONE;
    }
  if (node->inode_read)
    {
      info.mtimeset = 1;
      info.mtime = grub_le_to_cpu64 (node->inode.i.i_mtime);
    }

  info.dir = ((filetype & GRUB_FSHELP_TYPE_MASK) == GRUB_FSHELP_DIR);
  grub_free (node);

  return ctx->hook (filename, &info, ctx->hook_data);
}

static grub_err_t
grub_f2fs_dir (grub_device_t device, const char *path,
               grub_fs_dir_hook_t hook, void *hook_data)
{
  struct grub_f2fs_dir_ctx ctx = {
    .hook = hook,
    .hook_data = hook_data
  };
  struct grub_fshelp_node *fdiro = 0;

  grub_dl_ref (my_mod);

  ctx.data = grub_f2fs_mount (device->disk);
  if (!ctx.data)
    goto fail;

  grub_fshelp_find_file (path, &ctx.data->diropen, &fdiro,
                         grub_f2fs_iterate_dir, grub_f2fs_read_symlink,
                         GRUB_FSHELP_DIR);
  if (grub_errno)
    goto fail;

  grub_f2fs_iterate_dir (fdiro, grub_f2fs_dir_iter, &ctx);

 fail:
  if (fdiro != &ctx.data->diropen)
    grub_free (fdiro);
  grub_free (ctx.data);
  grub_dl_unref (my_mod);

  return grub_errno;
}

/* Open a file named NAME and initialize FILE. */
static grub_err_t
grub_f2fs_open (struct grub_file *file, const char *name)
{
  struct grub_f2fs_data *data = NULL;
  struct grub_fshelp_node *fdiro = 0;
  struct grub_f2fs_inode *inode;

  grub_dl_ref (my_mod);

  data = grub_f2fs_mount (file->device->disk);
  if (!data)
    goto fail;

  grub_fshelp_find_file (name, &data->diropen, &fdiro,
                         grub_f2fs_iterate_dir, grub_f2fs_read_symlink,
                         GRUB_FSHELP_REG);
  if (grub_errno)
    goto fail;

  if (!fdiro->inode_read)
    {
      grub_f2fs_read_node (data, fdiro->ino, &fdiro->inode);
      if (grub_errno)
        goto fail;
    }

  grub_memcpy (data->inode, &fdiro->inode, sizeof (*data->inode));
  grub_free (fdiro);

  inode = &(data->inode->i);
  file->size = grub_f2fs_file_size (inode);
  file->data = data;
  file->offset = 0;

  if (inode->i_inline & F2FS_INLINE_DATA && file->size > MAX_INLINE_DATA)
    grub_error (GRUB_ERR_BAD_FS, "corrupted inline_data: need fsck");

  return 0;

 fail:
  if (fdiro != &data->diropen)
    grub_free (fdiro);
  grub_free (data);

  grub_dl_unref (my_mod);

  return grub_errno;
}

static grub_ssize_t
grub_f2fs_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_f2fs_data *data = (struct grub_f2fs_data *) file->data;

  return grub_f2fs_read_file (&data->diropen,
                              file->read_hook, file->read_hook_data,
                              file->offset, len, buf);
}

static grub_err_t
grub_f2fs_close (grub_file_t file)
{
  struct grub_f2fs_data *data = (struct grub_f2fs_data *) file->data;

  grub_free (data);

  grub_dl_unref (my_mod);

  return GRUB_ERR_NONE;
}

static grub_uint8_t *
grub_f2fs_utf16_to_utf8 (grub_uint16_t *in_buf_le)
{
  grub_uint16_t in_buf[MAX_VOLUME_NAME];
  grub_uint8_t *out_buf;
  int len = 0;

  out_buf = grub_malloc (MAX_VOLUME_NAME * GRUB_MAX_UTF8_PER_UTF16 + 1);
  if (!out_buf)
    return NULL;

  while (*in_buf_le != 0 && len < MAX_VOLUME_NAME) {
    in_buf[len] = grub_le_to_cpu16 (in_buf_le[len]);
    len++;
  }

  *grub_utf16_to_utf8 (out_buf, in_buf, len) = '\0';

  return out_buf;
}

#if __GNUC__ >= 9
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

static grub_err_t
grub_f2fs_label (grub_device_t device, char **label)
{
  struct grub_f2fs_data *data;
  grub_disk_t disk = device->disk;

  grub_dl_ref (my_mod);

  data = grub_f2fs_mount (disk);
  if (data)
    *label = (char *) grub_f2fs_utf16_to_utf8 (data->sblock.volume_name);
  else
    *label = NULL;

  grub_free (data);
  grub_dl_unref (my_mod);

  return grub_errno;
}

#if __GNUC__ >= 9
#pragma GCC diagnostic pop
#endif

static grub_err_t
grub_f2fs_uuid (grub_device_t device, char **uuid)
{
  struct grub_f2fs_data *data;
  grub_disk_t disk = device->disk;

  grub_dl_ref (my_mod);

  data = grub_f2fs_mount (disk);
  if (data)
    {
      *uuid =
        grub_xasprintf
        ("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
         data->sblock.uuid[0], data->sblock.uuid[1],
         data->sblock.uuid[2], data->sblock.uuid[3],
         data->sblock.uuid[4], data->sblock.uuid[5],
         data->sblock.uuid[6], data->sblock.uuid[7],
         data->sblock.uuid[8], data->sblock.uuid[9],
         data->sblock.uuid[10], data->sblock.uuid[11],
         data->sblock.uuid[12], data->sblock.uuid[13],
         data->sblock.uuid[14], data->sblock.uuid[15]);
    }
  else
    *uuid = NULL;

  grub_free (data);
  grub_dl_unref (my_mod);

  return grub_errno;
}

static struct grub_fs grub_f2fs_fs = {
  .name                  = "f2fs",
  .fs_dir                   = grub_f2fs_dir,
  .fs_open                  = grub_f2fs_open,
  .fs_read                  = grub_f2fs_read,
  .fs_close                 = grub_f2fs_close,
  .fs_label                 = grub_f2fs_label,
  .fs_uuid                  = grub_f2fs_uuid,
#ifdef GRUB_UTIL
  .reserved_first_sector = 1,
  .blocklist_install     = 0,
#endif
  .next                  = 0
};

GRUB_MOD_INIT (f2fs)
{
  grub_f2fs_fs.mod = mod;
  grub_fs_register (&grub_f2fs_fs);
  my_mod = mod;
}

GRUB_MOD_FINI (f2fs)
{
  grub_fs_unregister (&grub_f2fs_fs);
}
