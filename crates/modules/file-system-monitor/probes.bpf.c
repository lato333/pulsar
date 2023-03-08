// SPDX-License-Identifier: GPL-2.0
#include "buffer.bpf.h"
#include "common.bpf.h"
#include "loop.bpf.h"
#include "output.bpf.h"
char LICENSE[] SEC("license") = "GPL";

#define FILE_CREATED 0
#define FILE_DELETED 1
#define DIR_CREATED 2
#define DIR_DELETED 3
#define FILE_OPENED 4
#define FILE_LINK 5
#define FILE_RENAME 6
#define MAX_PATH_COMPONENTS 20

struct file_opened_event {
  struct buffer_index filename;
  int flags;
};

struct file_link_event {
  struct buffer_index source;
  struct buffer_index destination;
  bool hard_link;
};

struct file_rename_event {
  struct buffer_index source;
  struct buffer_index destination;
};

OUTPUT_MAP(events, fs_event, {
  struct buffer_index created;
  struct buffer_index deleted;
  struct buffer_index dir_created;
  struct buffer_index dir_deleted;
  struct file_opened_event opened;
  struct file_link_event link;
  struct file_rename_event rename;
});

struct get_path_ctx {
  // Output of get_path_str
  struct buffer *buffer;
  struct buffer_index *index;

  // Current dentry being iterated
  struct dentry *dentry;
  struct vfsmount *vfsmnt;
  struct mount *mnt_p;
  struct mount *mnt_parent_p;

  // Internal list of path components, from dentry to the root
  const unsigned char *component_name[MAX_PATH_COMPONENTS];
  u32 component_len[MAX_PATH_COMPONENTS];
};

static __always_inline long get_dentry_name(u32 i, void *callback_ctx) {
  struct get_path_ctx *c = callback_ctx;
  if (!c)
    return 1;
  struct dentry *mnt_root = (struct dentry *)BPF_CORE_READ(c->vfsmnt, mnt_root);
  struct dentry *d_parent = BPF_CORE_READ(c->dentry, d_parent);
  // If a dentry is the parent of itself, or if it matches the root
  if (c->dentry == mnt_root || c->dentry == d_parent) {
    if (c->dentry != mnt_root) {
      // We reached root, but not mount root - escaped?
      return 1;
    }
    if (c->mnt_p != c->mnt_parent_p) {
      // We reached root, but not global root - continue with mount point
      c->dentry = BPF_CORE_READ(c->mnt_p, mnt_mountpoint);
      c->mnt_p = BPF_CORE_READ(c->mnt_p, mnt_parent);
      c->mnt_parent_p = BPF_CORE_READ(c->mnt_p, mnt_parent);
      c->vfsmnt = &c->mnt_p->mnt;
      return 0;
    }
    // Global root - path fully parsed
    return 1;
  }
  // Add this dentry name to path
  struct qstr entry = BPF_CORE_READ(c->dentry, d_name);
  if (i < MAX_PATH_COMPONENTS) {
    c->component_len[i] = entry.len;
    c->component_name[i] = entry.name;
  }
  c->dentry = d_parent;
  return 0;
}

// Build the full path by joining the output components of get_dentry_name.
// The loop starts from the end (t goes from (MAX_PATH_COMPONENTS-1) to 0)
// because the first component will always be the initial dentry.
static __always_inline long append_path_component(u32 i, void *callback_ctx) {
  struct get_path_ctx *c = callback_ctx;
  int t = MAX_PATH_COMPONENTS - i - 1;
  if (t < 0 || t >= MAX_PATH_COMPONENTS)
    return 1;
  char *name = (char *)c->component_name[t];
  int len = c->component_len[t];
  if (len == 0)
    return 0;
  buffer_append_str(c->buffer, c->index, "/", 1);
  buffer_append_str(c->buffer, c->index, name, len);
  return 0;
}

// Copy to buffer/index the path of the file pointed by dentry/path.
static void get_path_str(struct dentry *dentry, struct path *path,
                         struct buffer *buffer, struct buffer_index *index) {
  struct get_path_ctx c;
  c.index = index;
  c.buffer = buffer;
  __builtin_memset(c.component_name, 0, sizeof(c.component_name));
  __builtin_memset(c.component_len, 0, sizeof(c.component_len));
  c.dentry = dentry;
  c.vfsmnt = BPF_CORE_READ(path, mnt);
  c.mnt_p = container_of(c.vfsmnt, struct mount, mnt);
  c.mnt_parent_p = BPF_CORE_READ(c.mnt_p, mnt_parent);
  buffer_index_init(buffer, index);
  LOOP(MAX_PATH_COMPONENTS, get_dentry_name, &c);
  LOOP(MAX_PATH_COMPONENTS, append_path_component, &c);
  return;
}

PULSAR_LSM_HOOK(path_mknod, struct path *, dir, struct dentry *, dentry,
                umode_t, mode, unsigned int, dev);
static __always_inline void on_path_mknod(void *ctx, struct path *dir,
                                          struct dentry *dentry, umode_t mode,
                                          unsigned int dev) {
  struct fs_event *event = fs_event_init(FILE_CREATED);
  if (!event)
    return;
  get_path_str(dentry, dir, &event->buffer, &event->created);
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}

PULSAR_LSM_HOOK(path_unlink, struct path *, dir, struct dentry *, dentry);
static __always_inline void on_path_unlink(void *ctx, struct path *dir,
                                           struct dentry *dentry) {
  struct fs_event *event = fs_event_init(FILE_DELETED);
  if (!event)
    return;
  get_path_str(dentry, dir, &event->buffer, &event->deleted);
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}

PULSAR_LSM_HOOK(file_open, struct file *, file);
static __always_inline void on_file_open(void *ctx, struct file *file) {
  struct fs_event *event = fs_event_init(FILE_OPENED);
  if (!event)
    return;
  struct path path = BPF_CORE_READ(file, f_path);
  get_path_str(path.dentry, &path, &event->buffer, &event->opened.filename);
  event->opened.flags = BPF_CORE_READ(file, f_flags);
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}

PULSAR_LSM_HOOK(path_link, struct dentry *, old_dentry, struct path *, new_dir,
                struct dentry *, new_dentry);
static __always_inline void on_path_link(void *ctx, struct dentry *old_dentry,
                                         struct path *new_dir,
                                         struct dentry *new_dentry) {
  struct fs_event *event = fs_event_init(FILE_LINK);
  if (!event)
    return;
  get_path_str(new_dentry, new_dir, &event->buffer, &event->link.source);
  get_path_str(old_dentry, new_dir, &event->buffer, &event->link.destination);
  event->link.hard_link = true;
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}

PULSAR_LSM_HOOK(path_symlink, struct path *, dir, struct dentry *, dentry,
                char *, old_name);
static __always_inline void on_path_symlink(void *ctx, struct path *dir,
                                            struct dentry *dentry,
                                            char *old_name) {
  struct fs_event *event = fs_event_init(FILE_LINK);
  if (!event)
    return;
  get_path_str(dentry, dir, &event->buffer, &event->link.source);
  buffer_index_init(&event->buffer, &event->link.destination);
  buffer_append_str(&event->buffer, &event->link.destination, old_name,
                    BUFFER_MAX);
  event->link.hard_link = false;
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}

PULSAR_LSM_HOOK(path_mkdir, struct path *, dir, struct dentry *, dentry,
                umode_t, mode);
static __always_inline void on_path_mkdir(void *ctx, struct path *dir,
                                          struct dentry *dentry, umode_t mode) {
  struct fs_event *event = fs_event_init(DIR_CREATED);
  if (!event)
    return;
  get_path_str(dentry, dir, &event->buffer, &event->dir_created);
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}

PULSAR_LSM_HOOK(path_rmdir, struct path *, dir, struct dentry *, dentry);
static __always_inline void on_path_rmdir(void *ctx, struct path *dir,
                                          struct dentry *dentry) {
  struct fs_event *event = fs_event_init(DIR_DELETED);
  if (!event)
    return;
  get_path_str(dentry, dir, &event->buffer, &event->dir_deleted);
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}

PULSAR_LSM_HOOK(path_rename, struct path *, old_dir, struct dentry *,
                old_dentry, struct path *, new_dir, struct dentry *,
                new_dentry);
static __always_inline void on_path_rename(void *ctx, struct path *old_dir,
                                           struct dentry *old_dentry,
                                           struct path *new_dir,
                                           struct dentry *new_dentry) {
  struct fs_event *event = fs_event_init(FILE_RENAME);
  if (!event)
    return;
  get_path_str(old_dentry, old_dir, &event->buffer, &event->rename.source);
  get_path_str(new_dentry, new_dir, &event->buffer, &event->rename.destination);
  output_event(ctx, &events, event, sizeof(struct fs_event), event->buffer.len);
}