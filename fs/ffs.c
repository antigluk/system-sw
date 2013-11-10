#include "ffs.h"

#define FFS_FSINFO_INO 2
#define FFS_USERFILES_OFFSET 1
#define FFS_ROOT_INO FFS_USERFILES_OFFSET

static DEFINE_SPINLOCK(bitmap_fd_lock);
static DEFINE_SPINLOCK(bitmap_b_lock);

unsigned int last_ino = 3;

struct inode *root_inode = 0;
static struct kmem_cache *ffs_inode_cachep;

static ssize_t ffs_file_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);
static ssize_t ffs_file_read(struct file *file, char __user *buf, size_t max, loff_t *offset);

static ssize_t file_read(struct super_block *sb, struct inode *inode, char *buf, size_t max, loff_t *offset, int userspace);
struct inode *ffs_iget(struct super_block *sb, loff_t i_pos);

static struct inode *ffs_alloc_inode(struct super_block *sb);

const struct file_operations ffs_file_fops = {
    .read = ffs_file_read,
    .write = ffs_file_write,
};

// ====== UTILS =======

void _ffs_fd_copy(struct ffs_fd *to, struct ffs_fd *from)
{   // copy file descriptor
    // strcpy(to->filename, from->filename);
    to->link_count = from->link_count;
    to->file_size = from->file_size;
    to->type = from->type;
    to->datablock_id = from->datablock_id;
}

void kernel_msg(struct super_block *sb, const char *level, const char *fmt, ...)
{   // printf for kernel
    struct va_format vaf;
    va_list args;

    va_start(args, fmt);
    vaf.fmt = fmt;
    vaf.va = &args;
    printk("%sFFS (%s): %pV\n", level, sb->s_id, &vaf);
    va_end(args);
}

// ===== /UTILS =======

static inline struct ffs_inode_info *FFS_I(struct inode *inode)
{
    return container_of(inode, struct ffs_inode_info, vfs_inode);
}

struct dentry *ffs_lookup(struct inode *parent_inode, struct dentry *dentry, unsigned int flags)
{   // add inode to dentry
    struct super_block *sb = parent_inode->i_sb;
    // struct ffs_sb_info *sbi = sb->s_fs_info;
    // struct hlist_head *head = &sbi->inodes;
    // struct ffs_inode_info *i;

    loff_t offset;
    size_t dir_content_size;
    struct inode *inode;
    unsigned int i;
    struct ffs_directory_entry *dir_content, *iter;

    kernel_msg(sb, KERN_DEBUG, "lookup... %s", dentry->d_name.name);
    if (parent_inode->i_ino != FFS_ROOT_INO)
        // d_add(dentry, NULL);
        return ERR_PTR(-ENOENT);

    offset = 0;
    dir_content_size = FFS_I(parent_inode)->fd.file_size;
    dir_content = kzalloc(dir_content_size, GFP_KERNEL);
    while (file_read(sb, parent_inode, (char*)dir_content, dir_content_size, &offset, 0));

    iter = dir_content;
    for (i=0; i< (dir_content_size/sizeof(struct ffs_directory_entry)); i++ ) {
        if (strcmp(iter->filename, dentry->d_name.name) == 0) {
            if ((inode = ffs_iget(sb, FFS_USERFILES_OFFSET + iter->i_fd - 1))) { //-1 because numbering from 1
                kernel_msg(sb, KERN_DEBUG, "file #%d %s == %s ", iter->i_fd, iter->filename, dentry->d_name.name);
                d_add(dentry, inode);
                kfree(dir_content);
                return NULL;
            }
        }
        iter += 1;
    }

    kfree(dir_content);
    d_add(dentry, NULL);
    return NULL;
}

// ======== INODE ==========

struct inode *ffs_iget(struct super_block *sb, loff_t i_pos)
{
    struct ffs_sb_info *sbi = sb->s_fs_info;
    struct hlist_head *head = &sbi->inodes;
    struct ffs_inode_info *i;

    hlist_for_each_entry(i, head, list_node) {
        if (i->vfs_inode.i_ino == i_pos)
            return &i->vfs_inode;
    }
    return NULL;
}

static unsigned int ffs_get_empty_fd(struct super_block *sb)
{   //find empty fd and set bit in bitmask
    struct ffs_sb_info *sbi = sb->s_fs_info;
    unsigned int i, len;
    struct buffer_head *bh;
    for(i=0; i<sbi->max_file_count; i++) {
        if (!CHECK_BIT(sbi->fd_bitmask, i)) {
            
            spin_lock(&bitmap_fd_lock);
            SET_BIT(sbi->fd_bitmask, i);
            // mark dirty and sync BITMASK_PAGE(i) block
            bh = sb_bread(sb, BITMASK_PAGE(i) + FFS_FD_BITMASK_BLOCK(sbi));
            kernel_msg(sb, KERN_DEBUG, "BITMASK_FD block#%d", BITMASK_PAGE(i) + FFS_FD_BITMASK_BLOCK(sbi));
            kernel_msg(sb, KERN_DEBUG, "%X\t%X", *(char*)bh->b_data, *(char*)sbi->fd_bitmask);
            len = sbi->fd_bitmask_len - BITMASK_PAGE(i)*FFS_BLOCK_SIZE;
            len = (len>FFS_BLOCK_SIZE)?FFS_BLOCK_SIZE:len;
            memcpy(bh->b_data, sbi->fd_bitmask+BITMASK_PAGE(i)*FFS_BLOCK_SIZE, len);
            kernel_msg(sb, KERN_DEBUG, "copied %d bytes of bitmask", len);

            mark_buffer_dirty(bh);
            sync_dirty_buffer(bh);
            wait_on_buffer(bh);

            spin_unlock(&bitmap_fd_lock);
            return i + 1;
        }
    }
    return 0;
}

static unsigned int ffs_get_empty_block(struct super_block *sb)
{   //find empty block and set bit in bitmask
    struct ffs_sb_info *sbi = sb->s_fs_info;
    unsigned int i, len;
    struct buffer_head *bh;
    for(i=0; i<sbi->block_count; i++) {
        if (!CHECK_BIT(sbi->b_bitmask, i)) {

            spin_lock(&bitmap_b_lock);
            SET_BIT(sbi->b_bitmask, i);
            // mark dirty and sync BITMASK_PAGE(i) block
            bh = sb_bread(sb, BITMASK_PAGE(i) + FFS_B_BITMASK_BLOCK(sbi));
            kernel_msg(sb, KERN_DEBUG, "BITMASK_B block#%d", BITMASK_PAGE(i) + FFS_B_BITMASK_BLOCK(sbi));
            kernel_msg(sb, KERN_DEBUG, "%X\t%X", *(char*)bh->b_data, *(char*)sbi->b_bitmask);
            len = sbi->b_bitmask_len - BITMASK_PAGE(i)*FFS_BLOCK_SIZE;
            len = (len>FFS_BLOCK_SIZE)?FFS_BLOCK_SIZE:len;
            memcpy(bh->b_data, sbi->b_bitmask+BITMASK_PAGE(i)*FFS_BLOCK_SIZE, len);
            kernel_msg(sb, KERN_DEBUG, "copied %d bytes of bitmask", len);

            mark_buffer_dirty(bh);
            sync_dirty_buffer(bh);
            wait_on_buffer(bh);

            spin_unlock(&bitmap_b_lock);
            // mark dirty and sync BITMASK_PAGE(i) block
            return i + 1;
        }
    }
    return 0;
}

static int add_to_dir(struct inode *dir, struct inode *inode, struct dentry * dentry)
{

    return 0;
}

static int ffs_create (struct inode *dir, struct dentry * dentry,
                        umode_t mode, bool excl)
{   // create regular file
    struct inode *inode;
    struct ffs_sb_info *sbi = dir->i_sb->s_fs_info;
    int fd_i;

    inode = new_inode(dir->i_sb);

    inode->i_blocks = 0;
    fd_i = ffs_get_empty_fd(dir->i_sb);
    if (!fd_i) goto cleanup;
    inode->i_ino = FFS_USERFILES_OFFSET + (--fd_i);
    switch (mode & S_IFMT) {
        case S_IFDIR:
            // TODO: folder
            goto cleanup;
            break;
        default:
            inode->i_mode = mode | S_IFREG;
            kernel_msg(dir->i_sb, KERN_DEBUG, "create file... %s", dentry->d_name.name);

            // strcpy(FFS_I(inode)->fd.filename, dentry->d_name.name);
            FFS_I(inode)->fd.type = FFS_REG;
            FFS_I(inode)->fd.datablock_id = ffs_get_empty_block(dir->i_sb);
            FFS_I(inode)->fd.link_count = 1;
            FFS_I(inode)->fd.file_size = 0;
            kernel_msg(dir->i_sb, KERN_DEBUG, "new file inode#%d datablock#%d", fd_i, FFS_I(inode)->fd.datablock_id);

            inode->i_size = 0;
            // inode->i_mode = S_IFREG|S_IRUGO|S_IWUGO;
            inode->i_fop = &ffs_file_fops;

            hlist_add_head(&FFS_I(inode)->list_node, &sbi->inodes);

            add_to_dir(dir, inode, dentry);
            break;
    }

    d_instantiate(dentry, inode);  
    dget(dentry);

    return 0;
cleanup:
    iput(inode);
    return -EINVAL;
}

static const struct inode_operations ffs_dir_inode_operations = {
        .create         = ffs_create,
        .lookup         = ffs_lookup,
        // .mknod          = ffs_mknod,
        // .unlink         = ffs_unlink,
        // .mkdir          = ffs_mkdir,
        // .rmdir          = ffs_rmdir,
        // .rename         = ffs_rename,
        // .setattr        = ffs_setattr,
        // .getattr        = ffs_getattr,
};

//=========== FILE ==========

static ssize_t file_write(struct super_block *sb, struct inode *inode, const char *buf, size_t len, loff_t *ppos, int is_userspace)
{
    struct ffs_inode_info *f_inode = FFS_I(inode);
    unsigned int block_count = *(int*)(f_inode->datablock->b_data);
    unsigned int block_index = (*ppos) / FFS_BLOCK_SIZE;
    unsigned int last_block_num = *((int*)(f_inode->datablock->b_data)+block_index+1);
    unsigned int curr_blocks_rem_bytes = (block_index+1)*FFS_BLOCK_SIZE - (*ppos);
    kernel_msg(sb, KERN_DEBUG, "%d till #%d block end", curr_blocks_rem_bytes, block_index);
    return 0;
}

static ssize_t ffs_file_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct dentry *de = file->f_dentry;
    struct inode *inode = de->d_inode;
    struct super_block *sb = de->d_inode->i_sb;
    return file_write(sb, inode, buf, len, ppos, 1);
}

static ssize_t file_read(struct super_block *sb, struct inode *inode, char *buf, size_t max, loff_t *offset, int is_userspace)
{   // read from file
    struct ffs_inode_info *f_inode = FFS_I(inode);
    struct buffer_head *bh;
    unsigned int block_count, block_index, block_offset, block_num, len, able_to_read;
    signed int till_end;
    // kernel_msg(sb, KERN_DEBUG, "reading file, offset %d", (int)*offset);

    block_count = *(int*)(f_inode->datablock->b_data);
    block_index = (*offset) / FFS_BLOCK_SIZE;
    block_offset = (*offset)-(block_index*FFS_BLOCK_SIZE);
    block_num = *((int*)(f_inode->datablock->b_data)+block_index+1);
    kernel_msg(sb, KERN_DEBUG, "block %d/%d [%d], block_offset %d", block_index+1, block_count, block_num, block_offset);
    till_end = inode->i_size - block_index*FFS_BLOCK_SIZE - block_offset;
    if (till_end<0) {
        return 0;
    }
    bh = sb_bread(sb, block_num);
    able_to_read = FFS_BLOCK_SIZE-block_offset;
    // kernel_msg(sb, KERN_DEBUG, "%d till end", till_end);
    able_to_read = (till_end<able_to_read)?till_end:able_to_read;
    len = (able_to_read<=max)?able_to_read:max;
    if (is_userspace)
        copy_to_user(buf, bh->b_data, len);
    else
        memcpy(buf, bh->b_data, len);
    *offset += len;

    // brelse(bh);

    return len;
}

static ssize_t ffs_file_read(struct file *file, char __user *buf, size_t max, loff_t *offset)
{
    struct dentry *de = file->f_dentry;
    struct inode *inode = de->d_inode;
    struct super_block *sb = de->d_inode->i_sb;
    return file_read(sb, inode, buf, max, offset, 1);
    // return file_read(file, buf, max, offset, 1);
}

// directory ls
int ffs_f_readdir( struct file *file, void *dirent, filldir_t filldir ) {
    struct dentry *de = file->f_dentry;
    struct super_block *sb = de->d_inode->i_sb;
    loff_t offset;
    size_t dir_content_size;
    struct inode *inode;
    unsigned int i;
    struct ffs_directory_entry *dir_content, *iter;

    kernel_msg(de->d_sb, KERN_DEBUG, "ffs: file_operations.readdir called");
    if(file->f_pos > 0 )
        return 1;
    if(filldir(dirent, ".", 1, file->f_pos++, de->d_inode->i_ino, DT_DIR)||
       (filldir(dirent, "..", 2, file->f_pos++, de->d_parent->d_inode->i_ino, DT_DIR)))
        return 0;

    offset = 0;
    dir_content_size = FFS_I(de->d_inode)->fd.file_size;
    dir_content = kzalloc(dir_content_size, GFP_KERNEL);
    while (file_read(sb, de->d_inode, (char*)dir_content, dir_content_size, &offset, 0));

    iter = dir_content;
    for (i=0; i< (dir_content_size/sizeof(struct ffs_directory_entry)); i++ ) {
        kernel_msg(de->d_sb, KERN_DEBUG, "file #%d %s ", iter->i_fd, iter->filename);
        if ((inode = ffs_iget(sb, FFS_USERFILES_OFFSET + iter->i_fd - 1))) //-1 because numbering from 1
            if(filldir(dirent, iter->filename, FFS_FILENAME_LENGTH, file->f_pos++, inode->i_ino, DT_REG))
                return 0;
        iter += 1;
    }

    kfree(dir_content);
    return 1;
}


struct file_operations ffs_dir_fops = {
    readdir: &ffs_f_readdir
};

// ========== SUPER ===========

static struct inode *ffs_alloc_inode(struct super_block *sb)
{
    struct ffs_inode_info *ei;
    ei = (struct ffs_inode_info *)kmem_cache_alloc(ffs_inode_cachep, GFP_NOFS);
    if (!ei)
            return NULL;
    // inode_init_always(sb, &ei->vfs_inode);
    // ei->vfs_inode.i_sb = sb;
    kernel_msg(sb, KERN_DEBUG, "alloc inode");
    // ei->vfs_inode.i_state = 0;
    // insert_inode_hash(&ei->vfs_inode);
    return &ei->vfs_inode;
}

static void ffs_destroy_inode(struct inode *inode)
{
    kernel_msg(inode->i_sb, KERN_DEBUG, "destroy inode %u", (unsigned)inode->i_ino);
    kmem_cache_free(ffs_inode_cachep, FFS_I(inode));
}

static const struct super_operations ffs_sops = {
        .alloc_inode    = ffs_alloc_inode,
        .destroy_inode  = ffs_destroy_inode,
};

// reads initial state of FS from device
static int ffs_read_root(struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct ffs_sb_info *sbi = sb->s_fs_info;
    struct buffer_head *bh;
    int copied, b_id, i, j, to_copy, index;
    int fd_per_block = FFS_BLOCK_SIZE / sizeof(struct ffs_fd);
    struct ffs_fd *fd;
    struct inode *n_inode = NULL;
    kernel_msg(sb, KERN_DEBUG, "ffs_read_root");

    sbi->b_bitmask_len = (sbi->block_count + 7) / 8;
    sbi->b_bitmask = kzalloc(sbi->b_bitmask_len, GFP_KERNEL);
    kernel_msg(sb, KERN_DEBUG, "blocks bitmask: %d", sbi->b_bitmask_len);

    b_id = FFS_B_BITMASK_BLOCK(sbi);
    copied = 0;
    for(i=0; i<sbi->b_bm_blocks; i++) {
        bh = sb_bread(sb, b_id++);
        to_copy = sbi->b_bitmask_len - copied;
        if (to_copy >= FFS_BLOCK_SIZE)
            to_copy = FFS_BLOCK_SIZE;
        memcpy(sbi->b_bitmask + copied, bh->b_data, to_copy);
        copied -= to_copy;
        // brelse(bh);
    }

    //read from b_bm_blocks block file descriptors bitmask
    sbi->fd_bitmask_len = (sbi->max_file_count + 7) / 8;
    sbi->fd_bitmask = kzalloc(sbi->fd_bitmask_len, GFP_KERNEL);
    kernel_msg(sb, KERN_DEBUG, "fd bitmask: %d", sbi->fd_bitmask_len);
    
    b_id = FFS_FD_BITMASK_BLOCK(sbi);
    kernel_msg(sb, KERN_DEBUG, "reading fd bitmap, block:%d", b_id);
    copied = 0;
    for(i=0; i<sbi->fd_bm_blocks; i++) {
        bh = sb_bread(sb, b_id++);
        to_copy = sbi->fd_bitmask_len - copied;
        if (to_copy >= FFS_BLOCK_SIZE)
            to_copy = FFS_BLOCK_SIZE;
        memcpy(sbi->fd_bitmask + copied, bh->b_data, to_copy);
        copied -= to_copy;
        // brelse(bh);
    }

    //read descriptors
    index = 0;
    b_id = FFS_FD_LIST_BLOCK(sbi);
    for(i=0; i<sbi->fd_blocks; i++) {
        bh = sb_bread(sb, b_id++);
        fd = (struct ffs_fd *)bh->b_data;
        for(j=0; j<fd_per_block; j++) {
            if (index >= sbi->max_file_count) break;
            if (CHECK_BIT(sbi->fd_bitmask, index)) {
                kernel_msg(sb, KERN_DEBUG, "found %d file", index);
                if ((fd->type == FFS_DIR) && (index == 0)) {
                    kernel_msg(sb, KERN_DEBUG, "(is root directory)");
                    n_inode = root_inode;
                } else
                if (fd->type == FFS_REG) {
                    n_inode = new_inode(sb);
                    n_inode->i_ino = FFS_USERFILES_OFFSET + index;
                    kernel_msg(sb, KERN_DEBUG, "(is regular file)");
                    n_inode->i_mode = S_IFREG|S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH;
                    n_inode->i_fop = &ffs_file_fops;
                } else if (fd->type == FFS_DIR) {
                    kernel_msg(sb, KERN_DEBUG, "(is directory)");
                    //TODO
                }

                n_inode->i_size = fd->file_size;
                FFS_I(n_inode)->datablock = sb_bread(sb, fd->datablock_id);
                _ffs_fd_copy(&(FFS_I(n_inode)->fd), fd);

                hlist_add_head(&FFS_I(n_inode)->list_node, &sbi->inodes);
            }
            fd += 1;
            index++;
        }
        // brelse(bh);
    }
    return 0;
}

static int ffs_fill_super(struct super_block *sb, void *data, int silent)
{
    long error;
    struct ffs_sb_info *sbi;
    struct buffer_head *bh;
    struct ffs_metainfo_sector *metainfo;
    struct inode *fsinfo_inode = 0;
    kernel_msg(sb, KERN_DEBUG, "=======INIT============");
    kernel_msg(sb, KERN_DEBUG, "ffs_fill_super");
    /* filesystem information */
    sbi = kzalloc(sizeof(struct ffs_sb_info), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sb->s_fs_info = sbi;

    sb->s_magic = FFS_MAGIC;
    sb->s_op = &ffs_sops;
    sb->s_flags |= MS_NODIRATIME | MS_NOATIME;  // no modification time

    kernel_msg(sb, KERN_DEBUG, "reading info block");
    sb_min_blocksize(sb, FFS_BLOCK_SIZE);
    bh = sb_bread(sb, 0);
    // read metainfo to superblock-info
    metainfo = (struct ffs_metainfo_sector *) bh->b_data;

    if (metainfo->magic != cpu_to_le32(FFS_MAGIC)) {
        kernel_msg(sb, KERN_ERR, "Invalid filesystem (incorrect magic)");
        kernel_msg(sb, KERN_DEBUG, "magic: %X (required %X)", metainfo->magic, cpu_to_le32(FFS_MAGIC));
        brelse(bh);
        goto out_fail;
    } else {
        kernel_msg(sb, KERN_DEBUG, "magic: OK");
    }

    sb_set_blocksize(sb, metainfo->block_size);
    sbi->block_count = metainfo->block_count;
    sbi->block_size = metainfo->block_size;
    sbi->fd_blocks = metainfo->fd_blocks;
    sbi->max_file_count = metainfo->max_file_count;
    sbi->b_bm_blocks = metainfo->b_bm_blocks;
    sbi->fd_bm_blocks = metainfo->fd_bm_blocks;
    // brelse(bh);

    INIT_HLIST_HEAD(&sbi->inodes);

    // fill initial state of fs
    root_inode = new_inode(sb);
    if (!root_inode)
        return -ENOMEM; 
    root_inode->i_ino = FFS_ROOT_INO;
    root_inode->i_version = 1;
    root_inode->i_mode = S_IFDIR | S_IRUGO | S_IXUGO | S_IWUGO;
    root_inode->i_op = &ffs_dir_inode_operations;
    root_inode->i_fop = &ffs_dir_fops;
    error = ffs_read_root(root_inode);
    if (error < 0) {
        iput(root_inode);
        goto out_fail;
    }
    insert_inode_hash(root_inode);
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        kernel_msg(sb, KERN_ERR, "get root inode failed");
        goto out_fail;
    }
    kernel_msg(sb, KERN_DEBUG, "superblock generated");
    return 0;

out_fail:
        if (fsinfo_inode)
                iput(fsinfo_inode);
        if (root_inode)
                iput(root_inode);
        sb->s_fs_info = NULL;
        kfree(sbi);
        return error;
}

static struct dentry *ffs_mount(struct file_system_type *fs_type,
                        int flags, const char *dev_name,
                        void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, ffs_fill_super);
}

static void ffs_kill_block_super(struct super_block *sb)
{
    struct ffs_sb_info *sbi = sb->s_fs_info;
    struct hlist_head *head = &sbi->inodes;
    struct ffs_inode_info *i;

    kernel_msg(sb, KERN_DEBUG, "kill superblock");

    hlist_for_each_entry(i, head, list_node) {
        kernel_msg(sb, KERN_DEBUG, "inode %d", i->vfs_inode.i_ino);
        ffs_destroy_inode(&i->vfs_inode);
    }
    kernel_msg(sb, KERN_DEBUG, "========EXIT===========");
    // ffs_destroy_inode(root_inode);
}

static struct file_system_type ffs_fs_type = {
        .owner          = THIS_MODULE,
        .name           = "ffs",
        .mount          = ffs_mount,
        .kill_sb        = ffs_kill_block_super,
};
MODULE_ALIAS_FS("ffs");

static void init_once(void * foo)
{
    struct ffs_inode_info *ei = (struct ffs_inode_info *) foo;
    inode_init_once(&ei->vfs_inode);
}

static int init_inodecache(void)
{
        ffs_inode_cachep = kmem_cache_create("ffs_inode_cache",
                                             sizeof(struct ffs_inode_info),
                                             0, SLAB_RECLAIM_ACCOUNT,
                                             init_once);
        if (ffs_inode_cachep == NULL)
                return -ENOMEM;
        return 0;
}

static int __init init_ffs_fs(void)
{
    init_inodecache();
    return register_filesystem(&ffs_fs_type);
}

static void __exit exit_ffs_fs(void)
{
    unregister_filesystem(&ffs_fs_type);
    kmem_cache_destroy(ffs_inode_cachep);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roman Rader");
MODULE_DESCRIPTION("Flat FS");

module_init(init_ffs_fs)
module_exit(exit_ffs_fs)
