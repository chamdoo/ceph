// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "include/int_types.h"

#include <errno.h>
#include <limits.h>

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/ContextCompletion.h"
#include "common/Throttle.h"
#include "cls/lock/cls_lock_client.h"
#include "include/stringify.h"

#include "cls/rbd/cls_rbd.h"
#include "cls/rbd/cls_rbd_client.h"

#include "librbd/AioCompletion.h"
#include "librbd/AioRequest.h"
#include "librbd/AsyncFlattenRequest.h"
#include "librbd/AsyncResizeRequest.h"
#include "librbd/AsyncTrimRequest.h"
#include "librbd/CopyupRequest.h"
#include "librbd/DiffIterate.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageWatcher.h"
#include "librbd/internal.h"
#include "librbd/ObjectMap.h"
#include "librbd/parent_types.h"
#include "librbd/RebuildObjectMapRequest.h"
#include "include/util.h"

#include <boost/bind.hpp>
#include <boost/scope_exit.hpp>
#include "include/assert.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd: "

#define rbd_howmany(x, y)  (((x) + (y) - 1) / (y))

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;
// list binds to list() here, so std::list is explicitly used below

using ceph::bufferlist;
using librados::snap_t;
using librados::IoCtx;
using librados::Rados;

namespace librbd {

namespace {

int remove_object_map(ImageCtx *ictx) {
  assert(ictx->snap_lock.is_locked());
  CephContext *cct = ictx->cct;

  int r;
  for (std::map<snap_t, SnapInfo>::iterator it = ictx->snap_info.begin();
       it != ictx->snap_info.end(); ++it) {
    std::string oid(ObjectMap::object_map_name(ictx->id, it->first));
    r = ictx->md_ctx.remove(oid);
    if (r < 0 && r != -ENOENT) {
      lderr(cct) << "failed to remove object map " << oid << ": "
                 << cpp_strerror(r) << dendl;
      return r;
    }
  }

  r = ictx->md_ctx.remove(ObjectMap::object_map_name(ictx->id, CEPH_NOSNAP));
  if (r < 0 && r != -ENOENT) {
    lderr(cct) << "failed to remove object map: " << cpp_strerror(r) << dendl;
  }
  return 0;
}

int update_all_flags(ImageCtx *ictx, uint64_t flags, uint64_t mask) {
  assert(ictx->snap_lock.is_locked());
  CephContext *cct = ictx->cct;

  std::vector<uint64_t> snap_ids;
  snap_ids.push_back(CEPH_NOSNAP);
  for (std::map<snap_t, SnapInfo>::iterator it = ictx->snap_info.begin();
       it != ictx->snap_info.end(); ++it) {
    snap_ids.push_back(it->first);
  }

  for (size_t i=0; i<snap_ids.size(); ++i) {
    librados::ObjectWriteOperation op;
    cls_client::set_flags(&op, snap_ids[i], flags, mask);
    int r = ictx->md_ctx.operate(ictx->header_oid, &op);
    if (r < 0) {
      lderr(cct) << "failed to update image flags: " << cpp_strerror(r)
	         << dendl;
      return r;
    }
  }
  return 0;
}

int prepare_image_update(ImageCtx *ictx) {
  assert(ictx->owner_lock.is_locked() && !ictx->owner_lock.is_wlocked());
  if (ictx->image_watcher == NULL) {
    return -EROFS;
  } else if (!ictx->image_watcher->is_lock_supported() ||
             ictx->image_watcher->is_lock_owner()) {
    return 0;
  }

  // need to upgrade to a write lock
  int r = 0;
  bool acquired_lock = false;
  ictx->owner_lock.put_read();
  {
    RWLock::WLocker l(ictx->owner_lock);
    if (!ictx->image_watcher->is_lock_owner()) {
      r = ictx->image_watcher->try_lock();
      acquired_lock = ictx->image_watcher->is_lock_owner();
    }
  }
  if (acquired_lock) {
    // finish any AIO that was previously waiting on acquiring the
    // exclusive lock
    ictx->flush_async_operations();
  }
  ictx->owner_lock.get_read();
  return r;
}

int invoke_async_request(ImageCtx *ictx, const std::string& request_type,
                         bool permit_snapshot,
                         const boost::function<int(Context*)>& local_request,
                         const boost::function<int()>& remote_request) {
  int r;
  do {
    C_SaferCond ctx;
    {
      RWLock::RLocker l(ictx->owner_lock);
      {
        RWLock::RLocker snap_locker(ictx->snap_lock);
        if (ictx->read_only ||
            (!permit_snapshot && ictx->snap_id != CEPH_NOSNAP)) {
          return -EROFS;
        }
      }

      while (ictx->image_watcher->is_lock_supported()) {
        r = prepare_image_update(ictx);
        if (r < 0) {
          return -EROFS;
        } else if (ictx->image_watcher->is_lock_owner()) {
          break;
        }

        r = remote_request();
        if (r != -ETIMEDOUT && r != -ERESTART) {
          return r;
        }
        ldout(ictx->cct, 5) << request_type << " timed out notifying lock owner"
                            << dendl;
      }

      r = local_request(&ctx);
      if (r < 0) {
        return r;
      }
    }

    r = ctx.wait();
    if (r == -ERESTART) {
      ldout(ictx->cct, 5) << request_type << " interrupted: restarting"
                          << dendl;
    }
  } while (r == -ERESTART);
  return r;
}

} // anonymous namespace

  const string id_obj_name(const string &name)
  {
    return RBD_ID_PREFIX + name;
  }

  const string header_name(const string &image_id)
  {
    return RBD_HEADER_PREFIX + image_id;
  }

  const string old_header_name(const string &image_name)
  {
    return image_name + RBD_SUFFIX;
  }

  std::string unique_lock_name(const std::string &name, void *address) {
    return name + " (" + stringify(address) + ")";
  }

  int detect_format(IoCtx &io_ctx, const string &name,
		    bool *old_format, uint64_t *size)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    if (old_format)
      *old_format = true;
    int r = io_ctx.stat(old_header_name(name), size, NULL);
    if (r == -ENOENT) {
      if (old_format)
	*old_format = false;
      r = io_ctx.stat(id_obj_name(name), size, NULL);
      if (r < 0)
	return r;
    } else if (r < 0) {
      return r;
    }

    ldout(cct, 20) << "detect format of " << name << " : "
		   << (old_format ? (*old_format ? "old" : "new") :
		       "don't care")  << dendl;
    return 0;
  }

  bool has_parent(int64_t parent_pool_id, uint64_t off, uint64_t overlap)
  {
    return (parent_pool_id != -1 && off <= overlap);
  }

  void init_rbd_header(struct rbd_obj_header_ondisk& ondisk,
		       uint64_t size, int order, uint64_t bid)
  {
    uint32_t hi = bid >> 32;
    uint32_t lo = bid & 0xFFFFFFFF;
    uint32_t extra = rand() % 0xFFFFFFFF;
    memset(&ondisk, 0, sizeof(ondisk));

    memcpy(&ondisk.text, RBD_HEADER_TEXT, sizeof(RBD_HEADER_TEXT));
    memcpy(&ondisk.signature, RBD_HEADER_SIGNATURE,
	   sizeof(RBD_HEADER_SIGNATURE));
    memcpy(&ondisk.version, RBD_HEADER_VERSION, sizeof(RBD_HEADER_VERSION));

    snprintf(ondisk.block_name, sizeof(ondisk.block_name), "rb.%x.%x.%x",
	     hi, lo, extra);

    ondisk.image_size = size;
    ondisk.options.order = order;
    ondisk.options.crypt_type = RBD_CRYPT_NONE;
    ondisk.options.comp_type = RBD_COMP_NONE;
    ondisk.snap_seq = 0;
    ondisk.snap_count = 0;
    ondisk.reserved = 0;
    ondisk.snap_names_len = 0;
  }

  void image_info(ImageCtx *ictx, image_info_t& info, size_t infosize)
  {
    int obj_order = ictx->order;
    ictx->snap_lock.get_read();
    info.size = ictx->get_image_size(ictx->snap_id);
    ictx->snap_lock.put_read();
    info.obj_size = 1ULL << obj_order;
    info.num_objs = Striper::get_num_objects(ictx->layout, info.size);
    info.order = obj_order;
    memcpy(&info.block_name_prefix, ictx->object_prefix.c_str(),
	   min((size_t)RBD_MAX_BLOCK_NAME_SIZE,
	       ictx->object_prefix.length() + 1));
    // clear deprecated fields
    info.parent_pool = -1L;
    info.parent_name[0] = '\0';
  }

  uint64_t oid_to_object_no(const string& oid, const string& object_prefix)
  {
    istringstream iss(oid);
    // skip object prefix and separator
    iss.ignore(object_prefix.length() + 1);
    uint64_t num;
    iss >> std::hex >> num;
    return num;
  }

  int init_rbd_info(struct rbd_info *info)
  {
    memset(info, 0, sizeof(*info));
    return 0;
  }

  void trim_image(ImageCtx *ictx, uint64_t newsize, ProgressContext& prog_ctx)
  {
    assert(ictx->owner_lock.is_locked());
    assert(!ictx->image_watcher->is_lock_supported() ||
	   ictx->image_watcher->is_lock_owner());

    C_SaferCond ctx;
    ictx->snap_lock.get_read();
    AsyncTrimRequest *req = new AsyncTrimRequest(*ictx, &ctx, ictx->size,
						 newsize, prog_ctx);
    ictx->snap_lock.put_read();
    req->send();

    int r = ctx.wait();
    if (r < 0) {
      lderr(ictx->cct) << "warning: failed to remove some object(s): "
		       << cpp_strerror(r) << dendl;
    }
  }

  int read_rbd_info(IoCtx& io_ctx, const string& info_oid,
		    struct rbd_info *info)
  {
    int r;
    bufferlist bl;
    r = io_ctx.read(info_oid, bl, sizeof(*info), 0);
    if (r < 0)
      return r;
    if (r == 0) {
      return init_rbd_info(info);
    }

    if (r < (int)sizeof(*info))
      return -EIO;

    memcpy(info, bl.c_str(), r);
    return 0;
  }

  int read_header_bl(IoCtx& io_ctx, const string& header_oid,
		     bufferlist& header, uint64_t *ver)
  {
    int r;
    uint64_t off = 0;
#define READ_SIZE 4096
    do {
      bufferlist bl;
      r = io_ctx.read(header_oid, bl, READ_SIZE, off);
      if (r < 0)
	return r;
      header.claim_append(bl);
      off += r;
    } while (r == READ_SIZE);

    if (memcmp(RBD_HEADER_TEXT, header.c_str(), sizeof(RBD_HEADER_TEXT))) {
      CephContext *cct = (CephContext *)io_ctx.cct();
      lderr(cct) << "unrecognized header format" << dendl;
      return -ENXIO;
    }

    if (ver)
      *ver = io_ctx.get_last_version();

    return 0;
  }

  int notify_change(IoCtx& io_ctx, const string& oid, ImageCtx *ictx)
  {
    if (ictx) {
      ictx->refresh_lock.Lock();
      ldout(ictx->cct, 20) << "notify_change refresh_seq = " << ictx->refresh_seq
			   << " last_refresh = " << ictx->last_refresh << dendl;
      ++ictx->refresh_seq;
      ictx->refresh_lock.Unlock();
    }

    ImageWatcher::notify_header_update(io_ctx, oid);
    return 0;
  }

  int read_header(IoCtx& io_ctx, const string& header_oid,
		  struct rbd_obj_header_ondisk *header, uint64_t *ver)
  {
    bufferlist header_bl;
    int r = read_header_bl(io_ctx, header_oid, header_bl, ver);
    if (r < 0)
      return r;
    if (header_bl.length() < (int)sizeof(*header))
      return -EIO;
    memcpy(header, header_bl.c_str(), sizeof(*header));

    return 0;
  }

  int write_header(IoCtx& io_ctx, const string& header_oid, bufferlist& header)
  {
    bufferlist bl;
    int r = io_ctx.write(header_oid, header, header.length(), 0);

    notify_change(io_ctx, header_oid, NULL);

    return r;
  }

  int tmap_set(IoCtx& io_ctx, const string& imgname)
  {
    bufferlist cmdbl, emptybl;
    __u8 c = CEPH_OSD_TMAP_SET;
    ::encode(c, cmdbl);
    ::encode(imgname, cmdbl);
    ::encode(emptybl, cmdbl);
    return io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
  }

  int tmap_rm(IoCtx& io_ctx, const string& imgname)
  {
    bufferlist cmdbl;
    __u8 c = CEPH_OSD_TMAP_RM;
    ::encode(c, cmdbl);
    ::encode(imgname, cmdbl);
    return io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
  }

  void rollback_object(ImageCtx *ictx, uint64_t snap_id, const string& oid,
		       SimpleThrottle& throttle)
  {
    Context *req_comp = new C_SimpleThrottle(&throttle);
    librados::AioCompletion *rados_completion =
      librados::Rados::aio_create_completion(req_comp, NULL, rados_ctx_cb);
    librados::ObjectWriteOperation op;
    op.selfmanaged_snap_rollback(snap_id);
    ictx->data_ctx.aio_operate(oid, rados_completion, &op);
    ldout(ictx->cct, 10) << "scheduling selfmanaged_snap_rollback on "
                         << oid << " to " << snap_id << dendl;
    rados_completion->release();
  }

  int rollback_image(ImageCtx *ictx, uint64_t snap_id,
		     ProgressContext& prog_ctx)
  {
    uint64_t bsize = ictx->get_object_size();
    uint64_t numseg;
    {
      RWLock::RLocker l(ictx->snap_lock);
      numseg = Striper::get_num_objects(ictx->layout, ictx->get_current_size());
    }

    int r;
    CephContext *cct = ictx->cct;
    SimpleThrottle throttle(ictx->concurrent_management_ops, true);

    for (uint64_t i = 0; i < numseg; i++) {
      string oid = ictx->get_object_name(i);
      rollback_object(ictx, snap_id, ictx->get_object_name(i), throttle);
      prog_ctx.update_progress(i * bsize, numseg * bsize);
    }

    r = throttle.wait_for_ret();
    if (r < 0) {
      ldout(cct, 10) << "failed to rollback at least one object: "
		     << cpp_strerror(r) << dendl;
      return r;
    }

    {
      RWLock::WLocker l(ictx->snap_lock);
      ictx->object_map.rollback(snap_id);
    }
    return 0;
  }

  int list(IoCtx& io_ctx, vector<string>& names)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 20) << "list " << &io_ctx << dendl;

    bufferlist bl;
    int r = io_ctx.read(RBD_DIRECTORY, bl, 0, 0);
    if (r < 0)
      return r;

    // old format images are in a tmap
    if (bl.length()) {
      bufferlist::iterator p = bl.begin();
      bufferlist header;
      map<string,bufferlist> m;
      ::decode(header, p);
      ::decode(m, p);
      for (map<string,bufferlist>::iterator q = m.begin(); q != m.end(); ++q) {
	names.push_back(q->first);
      }
    }

    // new format images are accessed by class methods
    int max_read = 1024;
    string last_read = "";
    do {
      map<string, string> images;
      cls_client::dir_list(&io_ctx, RBD_DIRECTORY,
			   last_read, max_read, &images);
      for (map<string, string>::const_iterator it = images.begin();
	   it != images.end(); ++it) {
	names.push_back(it->first);
      }
      if (!images.empty()) {
	last_read = images.rbegin()->first;
      }
      r = images.size();
    } while (r == max_read);

    return 0;
  }

  int list_children(ImageCtx *ictx, set<pair<string, string> >& names)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "children list " << ictx->name << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    // no children for non-layered or old format image
    if (!ictx->test_features(RBD_FEATURE_LAYERING))
      return 0;

    parent_spec parent_spec(ictx->md_ctx.get_id(), ictx->id, ictx->snap_id);
    names.clear();

    // search all pools for children depending on this snapshot
    Rados rados(ictx->md_ctx);
    std::list<std::pair<int64_t, string> > pools;
    rados.pool_list2(pools);

    for (std::list<std::pair<int64_t, string> >::const_iterator it =
         pools.begin(); it != pools.end(); ++it) {
      int64_t base_tier;
      r = rados.pool_get_base_tier(it->first, &base_tier);
      if (r == -ENOENT) {
        ldout(cct, 1) << "pool " << it->second << " no longer exists" << dendl;
        continue;
      } else if (r < 0) {
        lderr(cct) << "Error retrieving base tier for pool " << it->second
                   << dendl;
	return r;
      }
      if (it->first != base_tier) {
	// pool is a cache; skip it
	continue;
      }

      IoCtx ioctx;
      r = rados.ioctx_create2(it->first, ioctx);
      if (r == -ENOENT) {
        ldout(cct, 1) << "pool " << it->second << " no longer exists" << dendl;
        continue;
      } else if (r < 0) {
        lderr(cct) << "Error accessing child image pool " << it->second
                   << dendl;
        return r;
      }

      set<string> image_ids;
      r = cls_client::get_children(&ioctx, RBD_CHILDREN, parent_spec,
                                   image_ids);
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "Error reading list of children from pool " << it->second
		   << dendl;
	return r;
      }

      for (set<string>::const_iterator id_it = image_ids.begin();
	   id_it != image_ids.end(); ++id_it) {
	string name;
	r = cls_client::dir_get_name(&ioctx, RBD_DIRECTORY,
				     *id_it, &name);
	if (r < 0) {
	  lderr(cct) << "Error looking up name for image id " << *id_it
		     << " in pool " << it->second << dendl;
	  return r;
	}
	names.insert(make_pair(it->second, name));
      }
    }

    return 0;
  }

  int snap_create(ImageCtx *ictx, const char *snap_name)
  {
    ldout(ictx->cct, 20) << "snap_create " << ictx << " " << snap_name << dendl;

    if (ictx->read_only) {
      return -EROFS;
    }

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    {
      RWLock::RLocker l(ictx->snap_lock);
      if (ictx->get_snap_id(snap_name) != CEPH_NOSNAP) {
        return -EEXIST;
      }
    }

    r = invoke_async_request(ictx, "snap_create", true,
                             boost::bind(&snap_create_helper, ictx, _1,
                                         snap_name),
                             boost::bind(&ImageWatcher::notify_snap_create,
                                         ictx->image_watcher, snap_name));
    if (r < 0 && r != -EEXIST) {
      return r;
    }

    ictx->perfcounter->inc(l_librbd_snap_create);
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return 0;
  }

  int snap_create_helper(ImageCtx* ictx, Context* ctx,
                         const char* snap_name) {
    assert(ictx->owner_lock.is_locked());
    assert(!ictx->image_watcher->is_lock_supported() ||
	   ictx->image_watcher->is_lock_owner());

    ldout(ictx->cct, 20) << "snap_create_helper " << ictx << " " << snap_name
                         << dendl;

    int r = ictx_check(ictx, true);
    if (r < 0) {
      return r;
    }

    RWLock::WLocker md_locker(ictx->md_lock);
    r = _flush(ictx);
    if (r < 0) {
      return r;
    }

    do {
      r = add_snap(ictx, snap_name);
    } while (r == -ESTALE);

    if (r < 0) {
      return r;
    }

    if (ctx != NULL) {
      ctx->complete(0);
    }
    return 0;
  }

  static int scan_for_parents(ImageCtx *ictx, parent_spec &pspec,
			      snapid_t oursnap_id)
  {
    if (pspec.pool_id != -1) {
      map<snap_t, SnapInfo>::iterator it;
      for (it = ictx->snap_info.begin();
	   it != ictx->snap_info.end(); ++it) {
	// skip our snap id (if checking base image, CEPH_NOSNAP won't match)
	if (it->first == oursnap_id)
	  continue;
	if (it->second.parent.spec == pspec)
	  break;
      }
      if (it == ictx->snap_info.end())
	return -ENOENT;
    }
    return 0;
  }

  int snap_remove(ImageCtx *ictx, const char *snap_name)
  {
    ldout(ictx->cct, 20) << "snap_remove " << ictx << " " << snap_name << dendl;

    if (ictx->read_only)
      return -EROFS;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    bool fast_diff_enabled = false;
    {
      RWLock::RLocker snap_locker(ictx->snap_lock);
      if (ictx->get_snap_id(snap_name) == CEPH_NOSNAP) {
        return -ENOENT;
      }
      fast_diff_enabled = ((ictx->features & RBD_FEATURE_FAST_DIFF) != 0);
    }

    if (fast_diff_enabled) {
      r = invoke_async_request(ictx, "snap_remove", true,
                               boost::bind(&snap_remove_helper, ictx, _1,
                                           snap_name),
                               boost::bind(&ImageWatcher::notify_snap_remove,
                                           ictx->image_watcher, snap_name));
      if (r < 0 && r != -EEXIST) {
        return r;
      }
    } else {
      RWLock::RLocker owner_lock(ictx->owner_lock);
      r = snap_remove_helper(ictx, NULL, snap_name);
      if (r < 0) {
        return r;
      }
    }

    notify_change(ictx->md_ctx, ictx->header_oid, ictx);

    ictx->perfcounter->inc(l_librbd_snap_remove);
    return 0;
  }

  int snap_remove_helper(ImageCtx *ictx, Context *ctx, const char *snap_name)
  {
    assert(ictx->owner_lock.is_locked());
    {
      if ((ictx->features & RBD_FEATURE_FAST_DIFF) != 0) {
        assert(!ictx->image_watcher->is_lock_supported() ||
               ictx->image_watcher->is_lock_owner());
      }
    }

    ldout(ictx->cct, 20) << "snap_remove_helper " << ictx << " " << snap_name
                         << dendl;

    int r = ictx_check(ictx, true);
    if (r < 0) {
      return r;
    }

    RWLock::RLocker md_locker(ictx->md_lock);
    snap_t snap_id;
    {
      RWLock::WLocker snap_locker(ictx->snap_lock);
      snap_id = ictx->get_snap_id(snap_name);
      if (snap_id == CEPH_NOSNAP) {
        return -ENOENT;
      }

      r = ictx->object_map.snapshot_remove(snap_id);
      if (r < 0) {
        lderr(ictx->cct) << "snap_remove: failed to remove snapshot object map"
		         << dendl;
        return r;
      }

      {
        parent_spec our_pspec;
        RWLock::RLocker parent_locker(ictx->parent_lock);
        r = ictx->get_parent_spec(snap_id, &our_pspec);
        if (r < 0) {
	  lderr(ictx->cct) << "snap_remove: can't get parent spec" << dendl;
	  return r;
        }

        if (ictx->parent_md.spec != our_pspec &&
	    (scan_for_parents(ictx, our_pspec, snap_id) == -ENOENT)) {
          r = cls_client::remove_child(&ictx->md_ctx, RBD_CHILDREN,
				       our_pspec, ictx->id);
	  if (r < 0 && r != -ENOENT) {
            lderr(ictx->cct) << "snap_remove: failed to deregister from parent "
                             << "image" << dendl;
	    return r;
          }
        }
      }

      r = rm_snap(ictx, snap_name, snap_id);
      if (r < 0) {
        return r;
      }
    }

    r = ictx->data_ctx.selfmanaged_snap_remove(snap_id);
    if (r < 0) {
      lderr(ictx->cct) << "snap_remove: failed to remove RADOS snapshot"
                       << dendl;
      return r;
    }

    if (ctx != NULL) {
      ctx->complete(0);
    }
    return 0;
  }

  int snap_protect(ImageCtx *ictx, const char *snap_name)
  {
    ldout(ictx->cct, 20) << "snap_protect " << ictx << " " << snap_name
			 << dendl;

    if (ictx->read_only)
      return -EROFS;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->md_lock);
    RWLock::RLocker l2(ictx->snap_lock);
    if ((ictx->features & RBD_FEATURE_LAYERING) == 0) {
      lderr(ictx->cct) << "snap_protect: image must support layering"
		       << dendl;
      return -ENOSYS;
    }
    snap_t snap_id = ictx->get_snap_id(snap_name);
    if (snap_id == CEPH_NOSNAP)
      return -ENOENT;

    bool is_protected;
    r = ictx->is_snap_protected(snap_id, &is_protected);
    if (r < 0)
      return r;

    if (is_protected)
      return -EBUSY;

    r = cls_client::set_protection_status(&ictx->md_ctx,
					  ictx->header_oid,
					  snap_id,
					  RBD_PROTECTION_STATUS_PROTECTED);
    if (r < 0)
      return r;
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return 0;
  }

  int snap_unprotect(ImageCtx *ictx, const char *snap_name)
  {
    ldout(ictx->cct, 20) << "snap_unprotect " << ictx << " " << snap_name
			 << dendl;

    if (ictx->read_only)
      return -EROFS;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->md_lock);
    RWLock::RLocker l2(ictx->snap_lock);
    if ((ictx->features & RBD_FEATURE_LAYERING) == 0) {
      lderr(ictx->cct) << "snap_unprotect: image must support layering"
		       << dendl;
      return -ENOSYS;
    }
    snap_t snap_id = ictx->get_snap_id(snap_name);
    if (snap_id == CEPH_NOSNAP)
      return -ENOENT;

    bool is_unprotected;
    r = ictx->is_snap_unprotected(snap_id, &is_unprotected);
    if (r < 0)
      return r;

    if (is_unprotected) {
      lderr(ictx->cct) << "snap_unprotect: snapshot is already unprotected"
		       << dendl;
      return -EINVAL;
    }

    r = cls_client::set_protection_status(&ictx->md_ctx,
					  ictx->header_oid,
					  snap_id,
					  RBD_PROTECTION_STATUS_UNPROTECTING);
    if (r < 0)
      return r;
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);

    parent_spec pspec(ictx->md_ctx.get_id(), ictx->id, snap_id);
    // search all pools for children depending on this snapshot
    Rados rados(ictx->md_ctx);
    rados.wait_for_latest_osdmap();

    // protect against pools being renamed/deleted
    std::list<std::pair<int64_t, std::string> > pools;
    rados.pool_list2(pools);

    for (std::list<std::pair<int64_t, std::string> >::const_iterator it =
         pools.begin(); it != pools.end(); ++it) {
      int64_t base_tier;
      r = rados.pool_get_base_tier(it->first, &base_tier);
      if (r == -ENOENT) {
        ldout(ictx->cct, 1) << "pool " << it->second << " no longer exists"
                            << dendl;
        continue;
      } else if (r < 0) {
        lderr(ictx->cct) << "snap_unprotect: error retrieving base tier for "
                         << "pool " << it->second << dendl;
        goto reprotect_and_return_err;
      }
      if (it->first != base_tier) {
	// pool is a cache; skip it
	continue;
      }

      IoCtx pool_ioctx;
      r = rados.ioctx_create2(it->first, pool_ioctx);
      if (r == -ENOENT) {
        ldout(ictx->cct, 1) << "pool " << it->second << " no longer exists"
                            << dendl;
        continue;
      } else if (r < 0) {
        lderr(ictx->cct) << "snap_unprotect: can't create ioctx for pool "
        		 << it->second << dendl;
        goto reprotect_and_return_err;
      }

      std::set<std::string> children;
      r = cls_client::get_children(&pool_ioctx, RBD_CHILDREN, pspec, children);
      // key should not exist for this parent if there is no entry
      if (((r < 0) && (r != -ENOENT))) {
        lderr(ictx->cct) << "can't get children for pool " << it->second
                         << dendl;
        goto reprotect_and_return_err;
      }
      // if we found a child, can't unprotect
      if (r == 0) {
        lderr(ictx->cct) << "snap_unprotect: can't unprotect; at least "
          << children.size() << " child(ren) in pool " << it->second << dendl;
        r = -EBUSY;
        goto reprotect_and_return_err;
      }
    }

    // didn't find any child in any pool, go ahead with unprotect
    r = cls_client::set_protection_status(&ictx->md_ctx,
					  ictx->header_oid,
					  snap_id,
					  RBD_PROTECTION_STATUS_UNPROTECTED);
    if (r < 0) {
      lderr(ictx->cct) << "snap_unprotect: error setting unprotected status"
		       << dendl;
      goto reprotect_and_return_err;
    }
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return 0;

reprotect_and_return_err:
    int proterr = cls_client::set_protection_status(&ictx->md_ctx,
						    ictx->header_oid,
						    snap_id,
					      RBD_PROTECTION_STATUS_PROTECTED);
    if (proterr < 0) {
      lderr(ictx->cct) << "snap_unprotect: can't reprotect image" << dendl;
    }
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return r;
  }

  int snap_is_protected(ImageCtx *ictx, const char *snap_name,
			bool *is_protected)
  {
    ldout(ictx->cct, 20) << "snap_is_protected " << ictx << " " << snap_name
			 << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    snap_t snap_id = ictx->get_snap_id(snap_name);
    if (snap_id == CEPH_NOSNAP)
      return -ENOENT;
    bool is_unprotected;
    r = ictx->is_snap_unprotected(snap_id, &is_unprotected);
    // consider both PROTECTED or UNPROTECTING to be 'protected',
    // since in either state they can't be deleted
    *is_protected = !is_unprotected;
    return r;
  }

  int create_v1(IoCtx& io_ctx, const char *imgname, uint64_t bid,
		uint64_t size, int order)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 2) << "adding rbd image to directory..." << dendl;
    int r = tmap_set(io_ctx, imgname);
    if (r < 0) {
      lderr(cct) << "error adding image to directory: " << cpp_strerror(r)
		 << dendl;
      return r;
    }

    ldout(cct, 2) << "creating rbd image..." << dendl;
    struct rbd_obj_header_ondisk header;
    init_rbd_header(header, size, order, bid);

    bufferlist bl;
    bl.append((const char *)&header, sizeof(header));

    string header_oid = old_header_name(imgname);
    r = io_ctx.write(header_oid, bl, bl.length(), 0);
    if (r < 0) {
      lderr(cct) << "Error writing image header: " << cpp_strerror(r)
		 << dendl;
      int remove_r = tmap_rm(io_ctx, imgname);
      if (remove_r < 0) {
	lderr(cct) << "Could not remove image from directory after "
		   << "header creation failed: "
		   << cpp_strerror(r) << dendl;
      }
      return r;
    }

    ldout(cct, 2) << "done." << dendl;
    return 0;
  }

  int create_v2(IoCtx& io_ctx, const char *imgname, uint64_t bid, uint64_t size,
		int order, uint64_t features, uint64_t stripe_unit,
		uint64_t stripe_count)
  {
    ostringstream bid_ss;
    uint32_t extra;
    string id, id_obj, header_oid;
    int remove_r;
    ostringstream oss;
    CephContext *cct = (CephContext *)io_ctx.cct();

    ceph_file_layout layout;

    id_obj = id_obj_name(imgname);

    int r = io_ctx.create(id_obj, true);
    if (r < 0) {
      lderr(cct) << "error creating rbd id object: " << cpp_strerror(r)
		 << dendl;
      return r;
    }

    extra = rand() % 0xFFFFFFFF;
    bid_ss << std::hex << bid << std::hex << extra;
    id = bid_ss.str();
    r = cls_client::set_id(&io_ctx, id_obj, id);
    if (r < 0) {
      lderr(cct) << "error setting image id: " << cpp_strerror(r) << dendl;
      goto err_remove_id;
    }

    ldout(cct, 2) << "adding rbd image to directory..." << dendl;
    r = cls_client::dir_add_image(&io_ctx, RBD_DIRECTORY, imgname, id);
    if (r < 0) {
      lderr(cct) << "error adding image to directory: " << cpp_strerror(r)
		 << dendl;
      goto err_remove_id;
    }

    oss << RBD_DATA_PREFIX << id;
    header_oid = header_name(id);
    r = cls_client::create_image(&io_ctx, header_oid, size, order,
				 features, oss.str());
    if (r < 0) {
      lderr(cct) << "error writing header: " << cpp_strerror(r) << dendl;
      goto err_remove_from_dir;
    }

    if ((stripe_unit || stripe_count) &&
	(stripe_count != 1 || stripe_unit != (1ull << order))) {
      r = cls_client::set_stripe_unit_count(&io_ctx, header_oid,
					    stripe_unit, stripe_count);
      if (r < 0) {
	lderr(cct) << "error setting striping parameters: "
		   << cpp_strerror(r) << dendl;
	goto err_remove_header;
      }
    }

    if ((features & RBD_FEATURE_FAST_DIFF) != 0 &&
        (features & RBD_FEATURE_OBJECT_MAP) == 0) {
      lderr(cct) << "cannot use fast diff without object map" << dendl;
      goto err_remove_header;
    } else if ((features & RBD_FEATURE_OBJECT_MAP) != 0) {
      if ((features & RBD_FEATURE_EXCLUSIVE_LOCK) == 0) {
        lderr(cct) << "cannot use object map without exclusive lock" << dendl;
        goto err_remove_header;
      }

      memset(&layout, 0, sizeof(layout));
      layout.fl_object_size = 1ull << order;
      if (stripe_unit == 0 || stripe_count == 0) {
        layout.fl_stripe_unit = layout.fl_object_size;
        layout.fl_stripe_count = 1;
      } else {
        layout.fl_stripe_unit = stripe_unit;
        layout.fl_stripe_count = stripe_count;
      }

      librados::ObjectWriteOperation op;
      cls_client::object_map_resize(&op, Striper::get_num_objects(layout, size),
                                    OBJECT_NONEXISTENT);
      r = io_ctx.operate(ObjectMap::object_map_name(id, CEPH_NOSNAP), &op);
      if (r < 0) {
        goto err_remove_header;
      }
    }

    ldout(cct, 2) << "done." << dendl;
    return 0;

  err_remove_header:
    remove_r = io_ctx.remove(header_oid);
    if (remove_r < 0) {
      lderr(cct) << "error cleaning up image header after creation failed: "
		 << dendl;
    }
  err_remove_from_dir:
    remove_r = cls_client::dir_remove_image(&io_ctx, RBD_DIRECTORY,
					    imgname, id);
    if (remove_r < 0) {
      lderr(cct) << "error cleaning up image from rbd_directory object "
		 << "after creation failed: " << cpp_strerror(remove_r)
		 << dendl;
    }
  err_remove_id:
    remove_r = io_ctx.remove(id_obj);
    if (remove_r < 0) {
      lderr(cct) << "error cleaning up id object after creation failed: "
		 << cpp_strerror(remove_r) << dendl;
    }

    return r;
  }

  int create(librados::IoCtx& io_ctx, const char *imgname, uint64_t size,
	     int *order)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    bool old_format = cct->_conf->rbd_default_format == 1;
    uint64_t features = old_format ? 0 : cct->_conf->rbd_default_features;
    return create(io_ctx, imgname, size, old_format, features, order, 0, 0);
  }

  int create(IoCtx& io_ctx, const char *imgname, uint64_t size,
	     bool old_format, uint64_t features, int *order,
	     uint64_t stripe_unit, uint64_t stripe_count)
  {
    if (!order)
      return -EINVAL;

    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 20) << "create " << &io_ctx << " name = " << imgname
		   << " size = " << size << " old_format = " << old_format
		   << " features = " << features << " order = " << *order
		   << " stripe_unit = " << stripe_unit
		   << " stripe_count = " << stripe_count
		   << dendl;


    if (features & ~RBD_FEATURES_ALL) {
      lderr(cct) << "librbd does not support requested features." << dendl;
      return -ENOSYS;
    }

    // make sure it doesn't already exist, in either format
    int r = detect_format(io_ctx, imgname, NULL, NULL);
    if (r != -ENOENT) {
      if (r) {
	lderr(cct) << "Could not tell if " << imgname << " already exists" << dendl;
	return r;
      }
      lderr(cct) << "rbd image " << imgname << " already exists" << dendl;
      return -EEXIST;
    }

    if (!*order)
      *order = cct->_conf->rbd_default_order;
    if (!*order)
      *order = RBD_DEFAULT_OBJ_ORDER;

    if (*order > 25 || *order < 12) {
      lderr(cct) << "order must be in the range [12, 25]" << dendl;
      return -EDOM;
    }

    Rados rados(io_ctx);
    uint64_t bid = rados.get_instance_id();

    // if striping is enabled, use possibly custom defaults
    if (!old_format && (features & RBD_FEATURE_STRIPINGV2) &&
	!stripe_unit && !stripe_count) {
      stripe_unit = cct->_conf->rbd_default_stripe_unit;
      stripe_count = cct->_conf->rbd_default_stripe_count;
    }

    // normalize for default striping
    if (stripe_unit == (1ull << *order) && stripe_count == 1) {
      stripe_unit = 0;
      stripe_count = 0;
    }
    if ((stripe_unit || stripe_count) &&
	(features & RBD_FEATURE_STRIPINGV2) == 0) {
      lderr(cct) << "STRIPINGV2 and format 2 or later required for non-default striping" << dendl;
      return -EINVAL;
    }
    if ((stripe_unit && !stripe_count) ||
	(!stripe_unit && stripe_count))
      return -EINVAL;

    if (old_format) {
      if (stripe_unit && stripe_unit != (1ull << *order))
	return -EINVAL;
      if (stripe_count && stripe_count != 1)
	return -EINVAL;

      return create_v1(io_ctx, imgname, bid, size, *order);
    } else {
      return create_v2(io_ctx, imgname, bid, size, *order, features,
		       stripe_unit, stripe_count);
    }
  }

  /*
   * Parent may be in different pool, hence different IoCtx
   */
  int clone(IoCtx& p_ioctx, const char *p_name, const char *p_snap_name,
	    IoCtx& c_ioctx, const char *c_name,
	    uint64_t features, int *c_order,
	    uint64_t stripe_unit, int stripe_count)
  {
    CephContext *cct = (CephContext *)p_ioctx.cct();
    ldout(cct, 20) << "clone " << &p_ioctx << " name " << p_name << " snap "
		   << p_snap_name << "to child " << &c_ioctx << " name "
		   << c_name << " features = " << features << " order = "
		   << *c_order
		   << " stripe_unit = " << stripe_unit
		   << " stripe_count = " << stripe_count
		   << dendl;

    if (features & ~RBD_FEATURES_ALL) {
      lderr(cct) << "librbd does not support requested features" << dendl;
      return -ENOSYS;
    }

    // make sure child doesn't already exist, in either format
    int r = detect_format(c_ioctx, c_name, NULL, NULL);
    if (r != -ENOENT) {
      lderr(cct) << "rbd image " << c_name << " already exists" << dendl;
      return -EEXIST;
    }

    if (p_snap_name == NULL) {
      lderr(cct) << "image to be cloned must be a snapshot" << dendl;
      return -EINVAL;
    }

    bool snap_protected;
    int order;
    uint64_t size;
    uint64_t p_features;
    int partial_r;
    librbd::NoOpProgressContext no_op;
    ImageCtx *c_imctx = NULL;
    map<string, bufferlist> pairs;
    // make sure parent snapshot exists
    ImageCtx *p_imctx = new ImageCtx(p_name, "", p_snap_name, p_ioctx, true);
    r = open_image(p_imctx);
    if (r < 0) {
      lderr(cct) << "error opening parent image: "
		 << cpp_strerror(-r) << dendl;
      return r;
    }

    parent_spec pspec(p_ioctx.get_id(), p_imctx->id,
				  p_imctx->snap_id);

    if (p_imctx->old_format) {
      lderr(cct) << "parent image must be in new format" << dendl;
      r = -EINVAL;
      goto err_close_parent;
    }

    p_imctx->snap_lock.get_read();
    p_features = p_imctx->features;
    size = p_imctx->get_image_size(p_imctx->snap_id);
    p_imctx->is_snap_protected(p_imctx->snap_id, &snap_protected);
    p_imctx->snap_lock.put_read();

    if ((p_features & RBD_FEATURE_LAYERING) != RBD_FEATURE_LAYERING) {
      lderr(cct) << "parent image must support layering" << dendl;
      r = -ENOSYS;
      goto err_close_parent;
    }

    if (!snap_protected) {
      lderr(cct) << "parent snapshot must be protected" << dendl;
      r = -EINVAL;
      goto err_close_parent;
    }

    order = *c_order;
    if (!order)
      order = p_imctx->order;

    r = create(c_ioctx, c_name, size, false, features, &order,
	       stripe_unit, stripe_count);
    if (r < 0) {
      lderr(cct) << "error creating child: " << cpp_strerror(r) << dendl;
      goto err_close_parent;
    }

    c_imctx = new ImageCtx(c_name, "", NULL, c_ioctx, false);
    r = open_image(c_imctx);
    if (r < 0) {
      lderr(cct) << "Error opening new image: " << cpp_strerror(r) << dendl;
      goto err_remove;
    }

    r = cls_client::set_parent(&c_ioctx, c_imctx->header_oid, pspec, size);
    if (r < 0) {
      lderr(cct) << "couldn't set parent: " << r << dendl;
      goto err_close_child;
    }

    r = cls_client::add_child(&c_ioctx, RBD_CHILDREN, pspec, c_imctx->id);
    if (r < 0) {
      lderr(cct) << "couldn't add child: " << r << dendl;
      goto err_close_child;
    }

    r = cls_client::metadata_list(&p_ioctx, p_imctx->header_oid, "", 0, &pairs);
    if (r < 0 && r != -EOPNOTSUPP && r != -EIO) {
      lderr(cct) << "couldn't list metadata: " << r << dendl;
      goto err_close_child;
    } else if (r == 0 && !pairs.empty()) {
      r = cls_client::metadata_set(&c_ioctx, c_imctx->header_oid, pairs);
      if (r < 0) {
        lderr(cct) << "couldn't set metadata: " << r << dendl;
        goto err_close_child;
      }
    }

    {
      RWLock::RLocker owner_locker(p_imctx->owner_lock);
      r = ictx_refresh(p_imctx);
    }
    if (r == 0) {
      p_imctx->snap_lock.get_read();
      r = p_imctx->is_snap_protected(p_imctx->snap_id, &snap_protected);
      p_imctx->snap_lock.put_read();
    }
    if (r < 0 || !snap_protected) {
      // we lost the race with unprotect
      r = -EINVAL;
      goto err_remove_child;
    }

    ldout(cct, 2) << "done." << dendl;
    r = close_image(c_imctx);
    partial_r = close_image(p_imctx);
    if (r == 0 && partial_r < 0) {
      r = partial_r;
    }
    return r;

  err_remove_child:
    partial_r = cls_client::remove_child(&c_ioctx, RBD_CHILDREN, pspec,
                                         c_imctx->id);
    if (partial_r < 0) {
     lderr(cct) << "Error removing failed clone from list of children: "
                << cpp_strerror(partial_r) << dendl;
    }
  err_close_child:
    close_image(c_imctx);
  err_remove:
    partial_r = remove(c_ioctx, c_name, no_op);
    if (partial_r < 0) {
      lderr(cct) << "Error removing failed clone: "
		 << cpp_strerror(partial_r) << dendl;
    }
  err_close_parent:
    close_image(p_imctx);
    return r;
  }

  int rename(IoCtx& io_ctx, const char *srcname, const char *dstname)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 20) << "rename " << &io_ctx << " " << srcname << " -> "
		   << dstname << dendl;

    bool old_format;
    uint64_t src_size;
    int r = detect_format(io_ctx, srcname, &old_format, &src_size);
    if (r < 0) {
      lderr(cct) << "error finding source object: " << cpp_strerror(r) << dendl;
      return r;
    }

    r = detect_format(io_ctx, dstname, NULL, NULL);
    if (r < 0 && r != -ENOENT) {
      lderr(cct) << "error checking for existing image called "
		 << dstname << ":" << cpp_strerror(r) << dendl;
      return r;
    }
    if (r == 0) {
      lderr(cct) << "rbd image " << dstname << " already exists" << dendl;
      return -EEXIST;
    }

    string src_oid =
      old_format ? old_header_name(srcname) : id_obj_name(srcname);
    string dst_oid =
      old_format ? old_header_name(dstname) : id_obj_name(dstname);

    string id;
    if (!old_format) {
      r = cls_client::get_id(&io_ctx, src_oid, &id);
      if (r < 0) {
	lderr(cct) << "error reading image id: " << cpp_strerror(r) << dendl;
	return r;
      }
    }

    bufferlist databl;
    map<string, bufferlist> omap_values;
    r = io_ctx.read(src_oid, databl, src_size, 0);
    if (r < 0) {
      lderr(cct) << "error reading source object: " << src_oid << ": "
		 << cpp_strerror(r) << dendl;
      return r;
    }

    int MAX_READ = 1024;
    string last_read = "";
    do {
      map<string, bufferlist> outbl;
      r = io_ctx.omap_get_vals(src_oid, last_read, MAX_READ, &outbl);
      if (r < 0) {
	lderr(cct) << "error reading source object omap values: "
		   << cpp_strerror(r) << dendl;
	return r;
      }
      omap_values.insert(outbl.begin(), outbl.end());
      if (!outbl.empty())
	last_read = outbl.rbegin()->first;
    } while (r == MAX_READ);

    librados::ObjectWriteOperation op;
    op.create(true);
    op.write_full(databl);
    if (!omap_values.empty())
      op.omap_set(omap_values);
    r = io_ctx.operate(dst_oid, &op);
    if (r < 0) {
      lderr(cct) << "error writing destination object: " << dst_oid << ": "
		 << cpp_strerror(r) << dendl;
      return r;
    }

    if (old_format) {
      r = tmap_set(io_ctx, dstname);
      if (r < 0) {
	io_ctx.remove(dst_oid);
	lderr(cct) << "couldn't add " << dstname << " to directory: "
		   << cpp_strerror(r) << dendl;
	return r;
      }
      r = tmap_rm(io_ctx, srcname);
      if (r < 0) {
	lderr(cct) << "warning: couldn't remove old entry from directory ("
		   << srcname << ")" << dendl;
      }
    } else {
      r = cls_client::dir_rename_image(&io_ctx, RBD_DIRECTORY,
				       srcname, dstname, id);
      if (r < 0) {
	lderr(cct) << "error updating directory: " << cpp_strerror(r) << dendl;
	return r;
      }
    }

    r = io_ctx.remove(src_oid);
    if (r < 0 && r != -ENOENT) {
      lderr(cct) << "warning: couldn't remove old source object ("
		 << src_oid << ")" << dendl;
    }

    if (old_format) {
      notify_change(io_ctx, old_header_name(srcname), NULL);
    }

    return 0;
  }


  int info(ImageCtx *ictx, image_info_t& info, size_t infosize)
  {
    ldout(ictx->cct, 20) << "info " << ictx << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    image_info(ictx, info, infosize);
    return 0;
  }

  int get_old_format(ImageCtx *ictx, uint8_t *old)
  {
    int r = ictx_check(ictx);
    if (r < 0)
      return r;
    *old = ictx->old_format;
    return 0;
  }

  int get_size(ImageCtx *ictx, uint64_t *size)
  {
    int r = ictx_check(ictx);
    if (r < 0)
      return r;
    RWLock::RLocker l2(ictx->snap_lock);
    *size = ictx->get_image_size(ictx->snap_id);
    return 0;
  }

  int get_features(ImageCtx *ictx, uint64_t *features)
  {
    int r = ictx_check(ictx);
    if (r < 0)
      return r;
    RWLock::RLocker l(ictx->snap_lock);
    *features = ictx->features;
    return 0;
  }

  int update_features(ImageCtx *ictx, uint64_t features, bool enabled)
  {
    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    CephContext *cct = ictx->cct;
    if (ictx->read_only) {
      return -EROFS;
    } else if (ictx->old_format) {
      lderr(cct) << "old-format images do not support features" << dendl;
      return -EINVAL;
    }

    if ((features & RBD_FEATURES_MUTABLE) != features) {
      lderr(cct) << "cannot update immutable features" << dendl;
      return -EINVAL;
    } else if (features == 0) {
      lderr(cct) << "update requires at least one feature" << dendl;
      return -EINVAL;
    }

    RWLock::RLocker l(ictx->snap_lock);
    uint64_t new_features = ictx->features | features;
    if (!enabled) {
      new_features = ictx->features & ~features;
    }

    if (ictx->features == new_features) {
      return 0;
    }

    uint64_t features_mask = features;
    uint64_t disable_flags = 0;
    if (enabled) {
      uint64_t enable_flags = 0;

      if ((features & RBD_FEATURE_OBJECT_MAP) != 0) {
        if ((new_features & RBD_FEATURE_EXCLUSIVE_LOCK) == 0) {
          lderr(cct) << "cannot enable object map" << dendl;
          return -EINVAL;
        }
        enable_flags |= RBD_FLAG_OBJECT_MAP_INVALID;
        features_mask |= RBD_FEATURE_EXCLUSIVE_LOCK;
      }
      if ((features & RBD_FEATURE_FAST_DIFF) != 0) {
        if ((new_features & RBD_FEATURE_OBJECT_MAP) == 0) {
          lderr(cct) << "cannot enable fast diff" << dendl;
          return -EINVAL;
        }
        enable_flags |= RBD_FLAG_FAST_DIFF_INVALID;
        features_mask |= (RBD_FEATURE_OBJECT_MAP | RBD_FEATURE_EXCLUSIVE_LOCK);
      }

      if (enable_flags != 0) {
        r = update_all_flags(ictx, enable_flags, enable_flags);
        if (r < 0) {
          return r;
        }
      }
    } else {
      if ((features & RBD_FEATURE_EXCLUSIVE_LOCK) != 0) {
        if ((new_features & RBD_FEATURE_OBJECT_MAP) != 0) {
          lderr(cct) << "cannot disable exclusive lock" << dendl;
          return -EINVAL;
        }
        features_mask |= RBD_FEATURE_OBJECT_MAP;
      }
      if ((features & RBD_FEATURE_OBJECT_MAP) != 0) {
        if ((new_features & RBD_FEATURE_FAST_DIFF) != 0) {
          lderr(cct) << "cannot disable object map" << dendl;
          return -EINVAL;
        }

        disable_flags = RBD_FLAG_OBJECT_MAP_INVALID;
        r = remove_object_map(ictx);
        if (r < 0) {
          lderr(cct) << "failed to remove object map" << dendl;
          return r;
        }
      }
      if ((features & RBD_FEATURE_FAST_DIFF) != 0) {
        disable_flags = RBD_FLAG_FAST_DIFF_INVALID;
      }
    }

    ldout(cct, 10) << "update_features: features=" << new_features << ", mask="
                   << features_mask << dendl;
    r = librbd::cls_client::set_features(&ictx->md_ctx, ictx->header_oid,
                                         new_features, features_mask);
    if (r < 0) {
      lderr(cct) << "failed to update features: " << cpp_strerror(r)
                 << dendl;
    }

    if (disable_flags != 0) {
      r = update_all_flags(ictx, 0, disable_flags);
      if (r < 0) {
        return r;
      }
    }

    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return 0;
  }

  int get_overlap(ImageCtx *ictx, uint64_t *overlap)
  {
    int r = ictx_check(ictx);
    if (r < 0)
      return r;
    RWLock::RLocker l(ictx->snap_lock);
    RWLock::RLocker l2(ictx->parent_lock);
    return ictx->get_parent_overlap(ictx->snap_id, overlap);
  }

  int open_parent(ImageCtx *ictx)
  {
    assert(ictx->cache_lock.is_locked());
    assert(ictx->snap_lock.is_wlocked());
    assert(ictx->parent_lock.is_wlocked());

    string pool_name;
    Rados rados(ictx->md_ctx);

    int64_t pool_id = ictx->get_parent_pool_id(ictx->snap_id);
    string parent_image_id = ictx->get_parent_image_id(ictx->snap_id);
    snap_t parent_snap_id = ictx->get_parent_snap_id(ictx->snap_id);
    assert(parent_snap_id != CEPH_NOSNAP);

    if (pool_id < 0)
      return -ENOENT;
    int r = rados.pool_reverse_lookup(pool_id, &pool_name);
    if (r < 0) {
      lderr(ictx->cct) << "error looking up name for pool id " << pool_id
		       << ": " << cpp_strerror(r) << dendl;
      return r;
    }

    IoCtx p_ioctx;
    r = rados.ioctx_create(pool_name.c_str(), p_ioctx);
    if (r < 0) {
      lderr(ictx->cct) << "error opening pool " << pool_name << ": "
		       << cpp_strerror(r) << dendl;
      return r;
    }

    // since we don't know the image and snapshot name, set their ids and
    // reset the snap_name and snap_exists fields after we read the header
    ictx->parent = new ImageCtx("", parent_image_id, NULL, p_ioctx, true);

    // set rados flags for reading the parent image
    if (ictx->balance_parent_reads)
      ictx->parent->set_read_flag(librados::OPERATION_BALANCE_READS);
    else if (ictx->localize_parent_reads)
      ictx->parent->set_read_flag(librados::OPERATION_LOCALIZE_READS);

    r = open_image(ictx->parent);
    if (r < 0) {
      lderr(ictx->cct) << "error opening parent image: " << cpp_strerror(r)
		       << dendl;
      ictx->parent = NULL;
      return r;
    }

    ictx->parent->cache_lock.Lock();
    ictx->parent->snap_lock.get_write();
    r = ictx->parent->get_snap_name(parent_snap_id, &ictx->parent->snap_name);
    if (r < 0) {
      lderr(ictx->cct) << "parent snapshot does not exist" << dendl;
      ictx->parent->snap_lock.put_write();
      ictx->parent->cache_lock.Unlock();
      close_image(ictx->parent);
      ictx->parent = NULL;
      return r;
    }
    ictx->parent->snap_set(ictx->parent->snap_name);
    ictx->parent->parent_lock.get_write();
    r = refresh_parent(ictx->parent);
    if (r < 0) {
      lderr(ictx->cct) << "error refreshing parent snapshot "
		       << ictx->parent->id << " "
		       << ictx->parent->snap_name << dendl;
      ictx->parent->parent_lock.put_write();
      ictx->parent->snap_lock.put_write();
      ictx->parent->cache_lock.Unlock();
      close_image(ictx->parent);
      ictx->parent = NULL;
      return r;
    }
    ictx->parent->parent_lock.put_write();
    ictx->parent->snap_lock.put_write();
    ictx->parent->cache_lock.Unlock();

    return 0;
  }

  int get_parent_info(ImageCtx *ictx, string *parent_pool_name,
		      string *parent_name, string *parent_snap_name)
  {
    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    RWLock::RLocker l2(ictx->parent_lock);
    if (ictx->parent == NULL) {
      return -ENOENT;
    }

    parent_spec parent_spec;

    if (ictx->snap_id == CEPH_NOSNAP) {
      if (!ictx->parent)
	return -ENOENT;
      parent_spec = ictx->parent_md.spec;
    } else {
      r = ictx->get_parent_spec(ictx->snap_id, &parent_spec);
      if (r < 0) {
	lderr(ictx->cct) << "Can't find snapshot id" << ictx->snap_id << dendl;
	return r;
      }
      if (parent_spec.pool_id == -1)
	return -ENOENT;
    }
    if (parent_pool_name) {
      Rados rados(ictx->md_ctx);
      r = rados.pool_reverse_lookup(parent_spec.pool_id,
				    parent_pool_name);
      if (r < 0) {
	lderr(ictx->cct) << "error looking up pool name" << cpp_strerror(r)
			 << dendl;
	return r;
      }
    }

    if (parent_snap_name) {
      RWLock::RLocker l(ictx->parent->snap_lock);
      r = ictx->parent->get_snap_name(parent_spec.snap_id,
				      parent_snap_name);
      if (r < 0) {
	lderr(ictx->cct) << "error finding parent snap name: "
			 << cpp_strerror(r) << dendl;
	return r;
      }
    }

    if (parent_name) {
      r = cls_client::dir_get_name(&ictx->parent->md_ctx, RBD_DIRECTORY,
				   parent_spec.image_id, parent_name);
      if (r < 0) {
	lderr(ictx->cct) << "error getting parent image name: "
			 << cpp_strerror(r) << dendl;
	return r;
      }
    }

    return 0;
  }

  int get_flags(ImageCtx *ictx, uint64_t *flags)
  {
    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    RWLock::RLocker l2(ictx->snap_lock);
    return ictx->get_flags(ictx->snap_id, flags);
  }

  int is_exclusive_lock_owner(ImageCtx *ictx, bool *is_owner)
  {
    RWLock::RLocker l(ictx->owner_lock);
    *is_owner = (ictx->image_watcher != NULL &&
		 ictx->image_watcher->is_lock_owner());
    return 0;
  }

  int remove(IoCtx& io_ctx, const char *imgname, ProgressContext& prog_ctx)
  {
    CephContext *cct((CephContext *)io_ctx.cct());
    ldout(cct, 20) << "remove " << &io_ctx << " " << imgname << dendl;

    string id;
    bool old_format = false;
    bool unknown_format = true;
    ImageCtx *ictx = new ImageCtx(imgname, "", NULL, io_ctx, false);
    int r = open_image(ictx);
    if (r < 0) {
      ldout(cct, 2) << "error opening image: " << cpp_strerror(-r) << dendl;
    } else {
      string header_oid = ictx->header_oid;
      old_format = ictx->old_format;
      unknown_format = false;
      id = ictx->id;

      ictx->owner_lock.get_read();
      if (ictx->image_watcher->is_lock_supported()) {
        r = prepare_image_update(ictx);
        if (r < 0 || !ictx->image_watcher->is_lock_owner()) {
	  lderr(cct) << "cannot obtain exclusive lock - not removing" << dendl;
	  ictx->owner_lock.put_read();
	  close_image(ictx);
          return -EBUSY;
        }
      }

      if (ictx->snaps.size()) {
	lderr(cct) << "image has snapshots - not removing" << dendl;
	ictx->owner_lock.put_read();
	close_image(ictx);
	return -ENOTEMPTY;
      }

      std::list<obj_watch_t> watchers;
      r = io_ctx.list_watchers(header_oid, &watchers);
      if (r < 0) {
        lderr(cct) << "error listing watchers" << dendl;
	ictx->owner_lock.put_read();
        close_image(ictx);
        return r;
      }
      if (watchers.size() > 1) {
        lderr(cct) << "image has watchers - not removing" << dendl;
	ictx->owner_lock.put_read();
        close_image(ictx);
        return -EBUSY;
      }

      trim_image(ictx, 0, prog_ctx);

      ictx->parent_lock.get_read();
      // struct assignment
      parent_info parent_info = ictx->parent_md;
      ictx->parent_lock.put_read();

      r = cls_client::remove_child(&ictx->md_ctx, RBD_CHILDREN,
				   parent_info.spec, id);
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error removing child from children list" << dendl;
	ictx->owner_lock.put_read();
        close_image(ictx);
	return r;
      }

      ictx->owner_lock.put_read();
      close_image(ictx);

      ldout(cct, 2) << "removing header..." << dendl;
      r = io_ctx.remove(header_oid);
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error removing header: " << cpp_strerror(-r) << dendl;
	return r;
      }
    }

    if (old_format || unknown_format) {
      ldout(cct, 2) << "removing rbd image from directory..." << dendl;
      r = tmap_rm(io_ctx, imgname);
      old_format = (r == 0);
      if (r < 0 && !unknown_format) {
	lderr(cct) << "error removing img from old-style directory: "
		   << cpp_strerror(-r) << dendl;
	return r;
      }
    }
    if (!old_format) {
      r = io_ctx.remove(ObjectMap::object_map_name(id, CEPH_NOSNAP));
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error removing image object map" << dendl;
      }

      ldout(cct, 2) << "removing id object..." << dendl;
      r = io_ctx.remove(id_obj_name(imgname));
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error removing id object: " << cpp_strerror(r) << dendl;
	return r;
      }

      r = cls_client::dir_get_id(&io_ctx, RBD_DIRECTORY, imgname, &id);
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error getting id of image" << dendl;
	return r;
      }

      ldout(cct, 2) << "removing rbd image from directory..." << dendl;
      r = cls_client::dir_remove_image(&io_ctx, RBD_DIRECTORY, imgname, id);
      if (r < 0) {
	lderr(cct) << "error removing img from new-style directory: "
		   << cpp_strerror(-r) << dendl;
	return r;
      }
    } 

    ldout(cct, 2) << "done." << dendl;
    return 0;
  }

  int resize(ImageCtx *ictx, uint64_t size, ProgressContext& prog_ctx)
  {
    CephContext *cct = ictx->cct;

    ictx->snap_lock.get_read();
    ldout(cct, 20) << "resize " << ictx << " " << ictx->size << " -> "
		   << size << dendl;
    ictx->snap_lock.put_read();

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    uint64_t request_id = ictx->async_request_seq.inc();
    r = invoke_async_request(ictx, "resize", false,
                             boost::bind(&async_resize, ictx, _1, size,
                                         boost::ref(prog_ctx)),
                             boost::bind(&ImageWatcher::notify_resize,
                                         ictx->image_watcher, request_id, size,
                                         boost::ref(prog_ctx)));

    ictx->perfcounter->inc(l_librbd_resize);
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    ldout(cct, 2) << "resize finished" << dendl;
    return r;
  }

  int async_resize(ImageCtx *ictx, Context *ctx, uint64_t size,
		   ProgressContext &prog_ctx)
  {
    assert(ictx->owner_lock.is_locked());
    assert(!ictx->image_watcher->is_lock_supported() ||
	   ictx->image_watcher->is_lock_owner());

    CephContext *cct = ictx->cct;
    ictx->snap_lock.get_read();
    ldout(cct, 20) << "async_resize " << ictx << " " << ictx->size << " -> "
		   << size << dendl;
    ictx->snap_lock.put_read();

    int r = ictx_check(ictx, true);
    if (r < 0) {
      return r;
    }

    {
      RWLock::RLocker snap_locker(ictx->snap_lock);
      if (ictx->snap_id != CEPH_NOSNAP || ictx->read_only) {
        return -EROFS;
      }
    }

    async_resize_helper(ictx, ctx, size, prog_ctx);
    return 0;
  }

  void async_resize_helper(ImageCtx *ictx, Context *ctx, uint64_t new_size,
                           ProgressContext& prog_ctx)
  {
    assert(ictx->owner_lock.is_locked());
    AsyncResizeRequest *req = new AsyncResizeRequest(*ictx, ctx, new_size,
                                                     prog_ctx);
    req->send();
  }

  int snap_list(ImageCtx *ictx, vector<snap_info_t>& snaps)
  {
    ldout(ictx->cct, 20) << "snap_list " << ictx << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;
    bufferlist bl, bl2;

    RWLock::RLocker l(ictx->snap_lock);
    for (map<snap_t, SnapInfo>::iterator it = ictx->snap_info.begin();
	 it != ictx->snap_info.end(); ++it) {
      snap_info_t info;
      info.name = it->second.name;
      info.id = it->first;
      info.size = it->second.size;
      snaps.push_back(info);
    }

    return 0;
  }

  bool snap_exists(ImageCtx *ictx, const char *snap_name)
  {
    ldout(ictx->cct, 20) << "snap_exists " << ictx << " " << snap_name << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    return ictx->get_snap_id(snap_name) != CEPH_NOSNAP;
  }


  int add_snap(ImageCtx *ictx, const char *snap_name)
  {
    assert(ictx->owner_lock.is_locked());
    assert(ictx->md_lock.is_wlocked());

    bool lock_owner = ictx->image_watcher->is_lock_owner();
    if (ictx->image_watcher->is_lock_supported()) {
      assert(lock_owner);
    }

    uint64_t snap_id;
    int r = ictx->md_ctx.selfmanaged_snap_create(&snap_id);
    if (r < 0) {
      lderr(ictx->cct) << "failed to create snap id: " << cpp_strerror(-r)
		       << dendl;
      return r;
    }

    if (ictx->old_format) {
      r = cls_client::old_snapshot_add(&ictx->md_ctx, ictx->header_oid,
				       snap_id, snap_name);
    } else {
      librados::ObjectWriteOperation op;
      if (lock_owner) {
	ictx->image_watcher->assert_header_locked(&op);
      }
      cls_client::snapshot_add(&op, snap_id, snap_name);
      r = ictx->md_ctx.operate(ictx->header_oid, &op);
    }

    if (r < 0) {
      lderr(ictx->cct) << "adding snapshot to header failed: "
		       << cpp_strerror(r) << dendl;
      ictx->data_ctx.selfmanaged_snap_remove(snap_id);
      return r;
    }

    RWLock::WLocker l(ictx->snap_lock);
    if (!ictx->old_format) {
      ictx->object_map.snapshot_add(snap_id);
      if (lock_owner) {
	// immediately start using the new snap context if we
	// own the exclusive lock
	std::vector<snapid_t> snaps;
	snaps.push_back(snap_id);
	snaps.insert(snaps.end(), ictx->snapc.snaps.begin(),
		     ictx->snapc.snaps.end());

	ictx->snapc.seq = snap_id;
	ictx->snapc.snaps.swap(snaps);
	ictx->data_ctx.selfmanaged_snap_set_write_ctx(ictx->snapc.seq,
						      ictx->snaps);
      }
    }
    return 0;
  }

  int rm_snap(ImageCtx *ictx, const char *snap_name, uint64_t snap_id)
  {
    assert(ictx->snap_lock.is_wlocked());

    int r;
    if (ictx->old_format) {
      r = cls_client::old_snapshot_remove(&ictx->md_ctx,
					  ictx->header_oid, snap_name);
    } else {
      r = cls_client::snapshot_remove(&ictx->md_ctx, ictx->header_oid, snap_id);
      if (r == 0) {
        ictx->rm_snap(snap_name, snap_id);
      }
    }

    if (r < 0) {
      lderr(ictx->cct) << "removing snapshot from header failed: "
		       << cpp_strerror(r) << dendl;
      return r;
    }

    return 0;
  }

  int ictx_check(ImageCtx *ictx, bool owner_locked)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "ictx_check " << ictx << dendl;

    ictx->refresh_lock.Lock();
    bool needs_refresh = ictx->last_refresh != ictx->refresh_seq;
    ictx->refresh_lock.Unlock();

    if (needs_refresh) {
      int r;
      if (owner_locked) {
        r = ictx_refresh(ictx);
      } else {
        RWLock::RLocker owner_lock(ictx->owner_lock);
        r = ictx_refresh(ictx);
      }
      if (r < 0) {
	lderr(cct) << "Error re-reading rbd header: " << cpp_strerror(-r)
		   << dendl;
	return r;
      }
    }
    return 0;
  }

  int refresh_parent(ImageCtx *ictx) {
    assert(ictx->cache_lock.is_locked());
    assert(ictx->snap_lock.is_wlocked());
    assert(ictx->parent_lock.is_wlocked());

    // close the parent if it changed or this image no longer needs
    // to read from it
    int r;
    if (ictx->parent) {
      uint64_t overlap;
      r = ictx->get_parent_overlap(ictx->snap_id, &overlap);
      if (r < 0 && r != -ENOENT) {
	return r;
      }
      if (r == -ENOENT || overlap == 0 ||
	  ictx->parent->md_ctx.get_id() !=
            ictx->get_parent_pool_id(ictx->snap_id) ||
	  ictx->parent->id != ictx->get_parent_image_id(ictx->snap_id) ||
	  ictx->parent->snap_id != ictx->get_parent_snap_id(ictx->snap_id)) {
	ictx->clear_nonexistence_cache();
	close_image(ictx->parent);
	ictx->parent = NULL;
      }
    }

    if (ictx->get_parent_pool_id(ictx->snap_id) > -1 && !ictx->parent) {
      r = open_parent(ictx);
      if (r < 0) {
	lderr(ictx->cct) << "error opening parent snapshot: "
			 << cpp_strerror(r) << dendl;
	return r;
      }
    }

    return 0;
  }

  int ictx_refresh(ImageCtx *ictx)
  {
    assert(ictx->owner_lock.is_locked());
    RWLock::WLocker md_locker(ictx->md_lock);

    CephContext *cct = ictx->cct;
    bufferlist bl, bl2;

    ldout(cct, 20) << "ictx_refresh " << ictx << dendl;

    ictx->refresh_lock.Lock();
    int refresh_seq = ictx->refresh_seq;
    ictx->refresh_lock.Unlock();

    ::SnapContext new_snapc;
    bool new_snap = false;
    vector<string> snap_names;
    vector<uint64_t> snap_sizes;
    vector<parent_info> snap_parents;
    vector<uint8_t> snap_protection;
    vector<uint64_t> snap_flags;
    {
      Mutex::Locker cache_locker(ictx->cache_lock);
      RWLock::WLocker snap_locker(ictx->snap_lock);
      {
	int r;
	RWLock::WLocker parent_locker(ictx->parent_lock);
	ictx->lockers.clear();
	if (ictx->old_format) {
	  r = read_header(ictx->md_ctx, ictx->header_oid, &ictx->header, NULL);
	  if (r < 0) {
	    lderr(cct) << "Error reading header: " << cpp_strerror(r) << dendl;
	    return r;
	  }
	  r = cls_client::old_snapshot_list(&ictx->md_ctx, ictx->header_oid,
					    &snap_names, &snap_sizes, &new_snapc);
	  if (r < 0) {
	    lderr(cct) << "Error listing snapshots: " << cpp_strerror(r)
		       << dendl;
	    return r;
	  }
	  ClsLockType lock_type = LOCK_NONE;
	  r = rados::cls::lock::get_lock_info(&ictx->md_ctx, ictx->header_oid,
					      RBD_LOCK_NAME, &ictx->lockers,
					      &lock_type, &ictx->lock_tag);

	  // If EOPNOTSUPP, treat image as if there are no locks (we can't
	  // query them).

	  // Ugly: OSDs prior to eed28daaf8927339c2ecae1b1b06c1b63678ab03
	  // return EIO when the class isn't present; should be EOPNOTSUPP.
	  // Treat EIO or EOPNOTSUPP the same for now, as LOCK_NONE.  Blech.

	  if (r < 0 && ((r != -EOPNOTSUPP) && (r != -EIO))) {
	    lderr(cct) << "Error getting lock info: " << cpp_strerror(r)
		       << dendl;
	    return r;
	  }
	  ictx->exclusive_locked = (lock_type == LOCK_EXCLUSIVE);
	  ictx->order = ictx->header.options.order;
	  ictx->size = ictx->header.image_size;
	  ictx->object_prefix = ictx->header.block_name;
	  ictx->init_layout();
	} else {
	  do {
	    uint64_t incompatible_features;
	    bool read_only = ictx->read_only || ictx->snap_id != CEPH_NOSNAP;
	    r = cls_client::get_mutable_metadata(&ictx->md_ctx, ictx->header_oid,
						 read_only,
						 &ictx->size, &ictx->features,
						 &incompatible_features,
						 &ictx->lockers,
						 &ictx->exclusive_locked,
						 &ictx->lock_tag,
						 &new_snapc,
						 &ictx->parent_md);
	    if (r < 0) {
	      lderr(cct) << "Error reading mutable metadata: " << cpp_strerror(r)
			 << dendl;
	      return r;
	    }

	    uint64_t unsupported = incompatible_features & ~RBD_FEATURES_ALL;
	    if (unsupported) {
	      lderr(ictx->cct) << "Image uses unsupported features: "
			       << unsupported << dendl;
	      return -ENOSYS;
	    }

	    r = cls_client::get_flags(&ictx->md_ctx, ictx->header_oid,
				      &ictx->flags, new_snapc.snaps,
				      &snap_flags);
	    if (r == -EOPNOTSUPP || r == -EIO) {
	      // Older OSD doesn't support RBD flags, need to assume the worst
	      ldout(ictx->cct, 10) << "OSD does not support RBD flags"
				   << "disabling object map optimizations"
				   << dendl;
	      ictx->flags = RBD_FLAG_OBJECT_MAP_INVALID;
              if ((ictx->features & RBD_FEATURE_FAST_DIFF) != 0) {
                ictx->flags |= RBD_FLAG_FAST_DIFF_INVALID;
              }

	      vector<uint64_t> default_flags(new_snapc.snaps.size(), ictx->flags);
	      snap_flags.swap(default_flags);
            } else if (r == -ENOENT) {
              ldout(ictx->cct, 10) << "Image at invalid snapshot" << dendl;
	      continue;
            } else if (r < 0) {
              lderr(cct) << "Error reading flags: " << cpp_strerror(r) << dendl;
              return r;
            }

	    r = cls_client::snapshot_list(&(ictx->md_ctx), ictx->header_oid,
					  new_snapc.snaps, &snap_names,
                                          &snap_sizes, &snap_parents,
                                          &snap_protection);
	    // -ENOENT here means we raced with snapshot deletion
	    if (r < 0 && r != -ENOENT) {
	      lderr(ictx->cct) << "snapc = " << new_snapc << dendl;
	      lderr(ictx->cct) << "Error listing snapshots: " << cpp_strerror(r)
			       << dendl;
	      return r;
	    }
	  } while (r == -ENOENT);
	}

	for (size_t i = 0; i < new_snapc.snaps.size(); ++i) {
	  parent_info parent;
	  if (!ictx->old_format)
	    parent = snap_parents[i];
	  vector<snap_t>::const_iterator it =
	    find(ictx->snaps.begin(), ictx->snaps.end(), new_snapc.snaps[i].val);
	  if (it == ictx->snaps.end()) {
	    new_snap = true;
	    ldout(cct, 20) << "new snapshot id=" << new_snapc.snaps[i].val
			   << " name=" << snap_names[i]
			   << " size=" << snap_sizes[i]
			   << dendl;
	  }
	}

	ictx->snaps.clear();
	ictx->snap_info.clear();
	ictx->snap_ids.clear();
	for (size_t i = 0; i < new_snapc.snaps.size(); ++i) {
	  uint64_t flags = ictx->old_format ? 0 : snap_flags[i];
	  uint8_t protection_status = ictx->old_format ?
	    (uint8_t)RBD_PROTECTION_STATUS_UNPROTECTED : snap_protection[i];
	  parent_info parent;
	  if (!ictx->old_format)
	    parent = snap_parents[i];
	  ictx->add_snap(snap_names[i], new_snapc.snaps[i].val, snap_sizes[i],
			 parent, protection_status, flags);
	}

	r = refresh_parent(ictx);
	if (r < 0)
	  return r;
      } // release parent_lock

      if (!new_snapc.is_valid()) {
	lderr(cct) << "image snap context is invalid!" << dendl;
	return -EIO;
      }

      ictx->snapc = new_snapc;

      if (ictx->snap_id != CEPH_NOSNAP &&
	  ictx->get_snap_id(ictx->snap_name) != ictx->snap_id) {
	lderr(cct) << "tried to read from a snapshot that no longer exists: "
		   << ictx->snap_name << dendl;
	ictx->snap_exists = false;
      }

      ictx->object_map.refresh(ictx->snap_id);

      ictx->data_ctx.selfmanaged_snap_set_write_ctx(ictx->snapc.seq, ictx->snaps);
    } // release snap_lock and cache_lock

    if (new_snap) {
      _flush(ictx);
    }

    ictx->refresh_lock.Lock();
    ictx->last_refresh = refresh_seq;
    ictx->refresh_lock.Unlock();

    return 0;
  }

  int snap_rollback(ImageCtx *ictx, const char *snap_name,
		    ProgressContext& prog_ctx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "snap_rollback " << ictx << " snap = " << snap_name
		   << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker owner_locker(ictx->owner_lock);
    snap_t snap_id;
    uint64_t new_size;
    {
      {
	// need to drop snap_lock before invalidating cache
	RWLock::RLocker snap_locker(ictx->snap_lock);
	if (!ictx->snap_exists) {
	  return -ENOENT;
	}

	if (ictx->snap_id != CEPH_NOSNAP || ictx->read_only) {
	  return -EROFS;
	}

	snap_id = ictx->get_snap_id(snap_name);
	if (snap_id == CEPH_NOSNAP) {
	  lderr(cct) << "No such snapshot found." << dendl;
	  return -ENOENT;
	}
      }

      r = prepare_image_update(ictx);
      if (r < 0) {
	return -EROFS;
      }
      if (ictx->image_watcher->is_lock_supported() &&
	  !ictx->image_watcher->is_lock_owner()) {
	return -EROFS;
      }

      ictx->snap_lock.get_read();
      new_size = ictx->get_image_size(snap_id);
      ictx->snap_lock.put_read();

      // need to flush any pending writes before resizing and rolling back -
      // writes might create new snapshots. Rolling back will replace
      // the current version, so we have to invalidate that too.
      RWLock::WLocker md_locker(ictx->md_lock);
      ictx->flush_async_operations();
      r = ictx->invalidate_cache();
      if (r < 0) {
	return r;
      }
    }

    ldout(cct, 2) << "resizing to snapshot size..." << dendl;
    NoOpProgressContext no_op;
    C_SaferCond ctx;
    async_resize_helper(ictx, &ctx, new_size, no_op);

    r = ctx.wait();
    if (r < 0) {
      lderr(cct) << "Error resizing to snapshot size: "
		 << cpp_strerror(r) << dendl;
      return r;
    }

    r = rollback_image(ictx, snap_id, prog_ctx);
    if (r < 0) {
      lderr(cct) << "Error rolling back image: " << cpp_strerror(-r) << dendl;
      return r;
    }

    notify_change(ictx->md_ctx, ictx->header_oid, ictx);

    ictx->perfcounter->inc(l_librbd_snap_rollback);
    return r;
  }

  struct CopyProgressCtx {
    CopyProgressCtx(ProgressContext &p)
      : destictx(NULL), src_size(0), prog_ctx(p)
    { }

    ImageCtx *destictx;
    uint64_t src_size;
    ProgressContext &prog_ctx;
  };

  int do_copy_extent(uint64_t offset, size_t len, const char *buf, void *data)
  {
    CopyProgressCtx *cp = reinterpret_cast<CopyProgressCtx*>(data);
    cp->prog_ctx.update_progress(offset, cp->src_size);
    int ret = 0;
    if (buf) {
      ret = write(cp->destictx, offset, len, buf, 0);
    }
    return ret;
  }

  int copy(ImageCtx *src, IoCtx& dest_md_ctx, const char *destname,
	   ProgressContext &prog_ctx)
  {
    CephContext *cct = (CephContext *)dest_md_ctx.cct();
    ldout(cct, 20) << "copy " << src->name
		   << (src->snap_name.length() ? "@" + src->snap_name : "")
		   << " -> " << destname << dendl;
    int order = src->order;

    src->snap_lock.get_read();
    uint64_t src_features = src->features;
    uint64_t src_size = src->get_image_size(src->snap_id);
    src->snap_lock.put_read();

    int r = create(dest_md_ctx, destname, src_size, src->old_format,
		   src_features, &order, src->stripe_unit, src->stripe_count);
    if (r < 0) {
      lderr(cct) << "header creation failed" << dendl;
      return r;
    }

    ImageCtx *dest = new librbd::ImageCtx(destname, "", NULL,
					  dest_md_ctx, false);
    r = open_image(dest);
    if (r < 0) {
      lderr(cct) << "failed to read newly created header" << dendl;
      return r;
    }

    r = copy(src, dest, prog_ctx);
    int close_r = close_image(dest);
    if (r == 0 && close_r < 0) {
      r = close_r;
    }
    return r;
  }

  class C_CopyWrite : public Context {
  public:
    C_CopyWrite(SimpleThrottle *throttle, bufferlist *bl)
      : m_throttle(throttle), m_bl(bl) {}
    virtual void finish(int r) {
      delete m_bl;
      m_throttle->end_op(r);
    }
  private:
    SimpleThrottle *m_throttle;
    bufferlist *m_bl;
  };

  class C_CopyRead : public Context {
  public:
    C_CopyRead(SimpleThrottle *throttle, ImageCtx *dest, uint64_t offset,
	       bufferlist *bl)
      : m_throttle(throttle), m_dest(dest), m_offset(offset), m_bl(bl) {
      m_throttle->start_op();
    }
    virtual void finish(int r) {
      if (r < 0) {
	lderr(m_dest->cct) << "error reading from source image at offset "
			   << m_offset << ": " << cpp_strerror(r) << dendl;
	delete m_bl;
	m_throttle->end_op(r);
	return;
      }
      assert(m_bl->length() == (size_t)r);

      if (m_bl->is_zero()) {
	delete m_bl;
	m_throttle->end_op(r);
	return;
      }

      Context *ctx = new C_CopyWrite(m_throttle, m_bl);
      AioCompletion *comp = aio_create_completion_internal(ctx, rbd_ctx_cb);
      aio_write(m_dest, m_offset, m_bl->length(), m_bl->c_str(), comp, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    }
  private:
    SimpleThrottle *m_throttle;
    ImageCtx *m_dest;
    uint64_t m_offset;
    bufferlist *m_bl;
  };

  int copy(ImageCtx *src, ImageCtx *dest, ProgressContext &prog_ctx)
  {
    src->snap_lock.get_read();
    uint64_t src_size = src->get_image_size(src->snap_id);
    src->snap_lock.put_read();

    dest->snap_lock.get_read();
    uint64_t dest_size = dest->get_image_size(dest->snap_id);
    dest->snap_lock.put_read();

    CephContext *cct = src->cct;
    if (dest_size < src_size) {
      lderr(cct) << " src size " << src_size << " >= dest size "
		 << dest_size << dendl;
      return -EINVAL;
    }
    int r;
    map<string, bufferlist> pairs;

    r = cls_client::metadata_list(&src->md_ctx, src->header_oid, "", 0, &pairs);
    if (r < 0 && r != -EOPNOTSUPP && r != -EIO) {
      lderr(cct) << "couldn't list metadata: " << r << dendl;
      return r;
    } else if (r == 0 && !pairs.empty()) {
      r = cls_client::metadata_set(&dest->md_ctx, dest->header_oid, pairs);
      if (r < 0) {
        lderr(cct) << "couldn't set metadata: " << r << dendl;
        return r;
      }
    }

    SimpleThrottle throttle(src->concurrent_management_ops, false);
    uint64_t period = src->get_stripe_period();
    unsigned fadvise_flags = LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL | LIBRADOS_OP_FLAG_FADVISE_NOCACHE;
    for (uint64_t offset = 0; offset < src_size; offset += period) {
      if (throttle.pending_error()) {
        return throttle.wait_for_ret();
      }

      uint64_t len = min(period, src_size - offset);
      bufferlist *bl = new bufferlist();
      Context *ctx = new C_CopyRead(&throttle, dest, offset, bl);
      AioCompletion *comp = aio_create_completion_internal(ctx, rbd_ctx_cb);
      aio_read(src, offset, len, NULL, bl, comp, fadvise_flags);
      prog_ctx.update_progress(offset, src_size);
    }

    r = throttle.wait_for_ret();
    if (r >= 0)
      prog_ctx.update_progress(src_size, src_size);
    return r;
  }

  // common snap_set functionality for snap_set and open_image

  int _snap_set(ImageCtx *ictx, const char *snap_name)
  {
    RWLock::WLocker owner_locker(ictx->owner_lock);
    RWLock::RLocker md_locker(ictx->md_lock);
    Mutex::Locker cache_locker(ictx->cache_lock);
    RWLock::WLocker snap_locker(ictx->snap_lock);
    RWLock::WLocker parent_locker(ictx->parent_lock);
    int r;
    if ((snap_name != NULL) && (strlen(snap_name) != 0)) {
      r = ictx->snap_set(snap_name);
    } else {
      ictx->snap_unset();
      r = 0;
    }
    if (r < 0) {
      return r;
    }
    refresh_parent(ictx);
    return 0;
  }

  int snap_set(ImageCtx *ictx, const char *snap_name)
  {
    ldout(ictx->cct, 20) << "snap_set " << ictx << " snap = "
			 << (snap_name ? snap_name : "NULL") << dendl;

    // ignore return value, since we may be set to a non-existent
    // snapshot and the user is trying to fix that
    ictx_check(ictx);

    bool unlocking = false;
    {
      RWLock::WLocker l(ictx->owner_lock);
      if (ictx->image_watcher != NULL && ictx->image_watcher->is_lock_owner() &&
          snap_name != NULL && strlen(snap_name) != 0) {
        // stop incoming requests since we will release the lock
        ictx->image_watcher->prepare_unlock();
        unlocking = true;
      }
    }

    ictx->cancel_async_requests();
    ictx->flush_async_operations();
    if (ictx->object_cacher) {
      // complete pending writes before we're set to a snapshot and
      // get -EROFS for writes
      RWLock::RLocker owner_locker(ictx->owner_lock);
      RWLock::WLocker md_locker(ictx->md_lock);
      ictx->flush_cache();
    }
    int r = _snap_set(ictx, snap_name);
    if (r < 0) {
      RWLock::WLocker l(ictx->owner_lock);
      if (unlocking) {
        ictx->image_watcher->cancel_unlock();
      }
      return r;
    }

    RWLock::WLocker l(ictx->owner_lock);
    if (ictx->image_watcher != NULL) {
      if (unlocking) {
	r = ictx->image_watcher->unlock();
	if (r < 0) {
	  lderr(ictx->cct) << "error unlocking image: " << cpp_strerror(r)
                           << dendl;
	}
      }
    }
    return r;
  }

  int open_image(ImageCtx *ictx)
  {
    ldout(ictx->cct, 20) << "open_image: ictx = " << ictx
			 << " name = '" << ictx->name
			 << "' id = '" << ictx->id
			 << "' snap_name = '"
			 << ictx->snap_name << "'" << dendl;
    int r = ictx->init();
    if (r < 0)
      goto err_close;

    if (!ictx->read_only) {
      r = ictx->register_watch();
      if (r < 0) {
	lderr(ictx->cct) << "error registering a watch: " << cpp_strerror(r)
			 << dendl;
	goto err_close;
      }
    }

    {
      RWLock::RLocker owner_locker(ictx->owner_lock);
      r = ictx_refresh(ictx);
    }
    if (r < 0)
      goto err_close;

    if ((r = _snap_set(ictx, ictx->snap_name.c_str())) < 0)
      goto err_close;

    return 0;

  err_close:
    close_image(ictx);
    return r;
  }

  int close_image(ImageCtx *ictx)
  {
    ldout(ictx->cct, 20) << "close_image " << ictx << dendl;

    {
      RWLock::WLocker l(ictx->owner_lock);
      if (ictx->image_watcher != NULL && ictx->image_watcher->is_lock_owner()) {
        // stop incoming requests
        ictx->image_watcher->prepare_unlock();
      }
    }

    ictx->aio_work_queue->drain();
    ictx->cancel_async_requests();
    ictx->flush_async_operations();
    ictx->readahead.wait_for_pending();

    int r;
    if (ictx->object_cacher) {
      r = ictx->shutdown_cache(); // implicitly flushes
    } else {
      r = flush(ictx);
    }
    if (r < 0) {
      lderr(ictx->cct) << "error flushing IO: " << cpp_strerror(r)
                       << dendl;
    }

    ictx->op_work_queue->drain();

    if (ictx->copyup_finisher != NULL) {
      ictx->copyup_finisher->wait_for_empty();
      ictx->copyup_finisher->stop();
    }

    if (ictx->parent) {
      int close_r = close_image(ictx->parent);
      if (r == 0 && close_r < 0) {
        r = close_r;
      }
      ictx->parent = NULL;
    }

    if (ictx->image_watcher) {
      {
	RWLock::WLocker l(ictx->owner_lock);
	if (ictx->image_watcher->is_lock_owner()) {
	  int unlock_r = ictx->image_watcher->unlock();
	  if (unlock_r < 0) {
	    lderr(ictx->cct) << "error unlocking image: "
                             << cpp_strerror(unlock_r) << dendl;
            if (r == 0) {
              r = unlock_r;
            }
	  }
	}
      }
      ictx->unregister_watch();
    }

    delete ictx;
    return r;
  }

  // 'flatten' child image by copying all parent's blocks
  int flatten(ImageCtx *ictx, ProgressContext &prog_ctx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "flatten" << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    if (ictx->read_only) {
      return -EROFS;
    }

    {
      RWLock::RLocker parent_locker(ictx->parent_lock);
      if (ictx->parent_md.spec.pool_id == -1) {
	lderr(cct) << "image has no parent" << dendl;
	return -EINVAL;
      }
    }

    uint64_t request_id = ictx->async_request_seq.inc();
    r = invoke_async_request(ictx, "flatten", false,
                             boost::bind(&async_flatten, ictx, _1,
                                         boost::ref(prog_ctx)),
                             boost::bind(&ImageWatcher::notify_flatten,
                                         ictx->image_watcher, request_id,
                                         boost::ref(prog_ctx)));

    if (r < 0 && r != -EINVAL) {
      return r;
    }

    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    ldout(cct, 20) << "flatten finished" << dendl;
    return 0;
  }

  int async_flatten(ImageCtx *ictx, Context *ctx, ProgressContext &prog_ctx)
  {
    assert(ictx->owner_lock.is_locked());
    assert(!ictx->image_watcher->is_lock_supported() ||
	   ictx->image_watcher->is_lock_owner());

    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "flatten" << dendl;

    int r;
    // ictx_check also updates parent data
    if ((r = ictx_check(ictx, true)) < 0) {
      lderr(cct) << "ictx_check failed" << dendl;
      return r;
    }

    uint64_t object_size;
    uint64_t overlap_objects;
    ::SnapContext snapc;

    {
      uint64_t overlap;
      RWLock::RLocker l(ictx->snap_lock);
      RWLock::RLocker l2(ictx->parent_lock);

      if (ictx->read_only) {
        return -EROFS;
      }

      // can't flatten a non-clone
      if (ictx->parent_md.spec.pool_id == -1) {
	lderr(cct) << "image has no parent" << dendl;
	return -EINVAL;
      }
      if (ictx->snap_id != CEPH_NOSNAP || ictx->read_only) {
	lderr(cct) << "snapshots cannot be flattened" << dendl;
	return -EROFS;
      }

      snapc = ictx->snapc;
      assert(ictx->parent != NULL);
      r = ictx->get_parent_overlap(CEPH_NOSNAP, &overlap);
      assert(r == 0);
      assert(overlap <= ictx->size);

      object_size = ictx->get_object_size();
      overlap_objects = Striper::get_num_objects(ictx->layout, overlap);
    }

    AsyncFlattenRequest *req =
      new AsyncFlattenRequest(*ictx, ctx, object_size, overlap_objects,
			      snapc, prog_ctx);
    req->send();
    return 0;
  }

  int rebuild_object_map(ImageCtx *ictx, ProgressContext &prog_ctx) {
    CephContext *cct = ictx->cct;
    ldout(cct, 10) << "rebuild_object_map" << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    uint64_t request_id = ictx->async_request_seq.inc();
    r = invoke_async_request(ictx, "rebuild object map", true,
                             boost::bind(&async_rebuild_object_map, ictx, _1,
                                         boost::ref(prog_ctx)),
                             boost::bind(&ImageWatcher::notify_rebuild_object_map,
                                         ictx->image_watcher, request_id,
                                         boost::ref(prog_ctx)));

    ldout(cct, 10) << "rebuild object map finished" << dendl;
    if (r < 0) {
      notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    }
    return r;
  }

  int async_rebuild_object_map(ImageCtx *ictx, Context *ctx,
                               ProgressContext &prog_ctx) {
    assert(ictx->owner_lock.is_locked());
    assert(!ictx->image_watcher->is_lock_supported() ||
	   ictx->image_watcher->is_lock_owner());

    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "async_rebuild_object_map " << ictx << dendl;

    if (ictx->read_only) {
      return -EROFS;
    }
    if (!ictx->test_features(RBD_FEATURE_OBJECT_MAP)) {
      return -EINVAL;
    }

    int r = ictx_check(ictx, true);
    if (r < 0) {
      return r;
    }

    RebuildObjectMapRequest *req = new RebuildObjectMapRequest(*ictx, ctx,
                                                               prog_ctx);
    req->send();
    return 0;
  }

  int list_lockers(ImageCtx *ictx,
		   std::list<locker_t> *lockers,
		   bool *exclusive,
		   string *tag)
  {
    ldout(ictx->cct, 20) << "list_locks on image " << ictx << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker locker(ictx->md_lock);
    if (exclusive)
      *exclusive = ictx->exclusive_locked;
    if (tag)
      *tag = ictx->lock_tag;
    if (lockers) {
      lockers->clear();
      map<rados::cls::lock::locker_id_t,
	  rados::cls::lock::locker_info_t>::const_iterator it;
      for (it = ictx->lockers.begin(); it != ictx->lockers.end(); ++it) {
	locker_t locker;
	locker.client = stringify(it->first.locker);
	locker.cookie = it->first.cookie;
	locker.address = stringify(it->second.addr);
	lockers->push_back(locker);
      }
    }

    return 0;
  }

  int lock(ImageCtx *ictx, bool exclusive, const string& cookie,
	   const string& tag)
  {
    ldout(ictx->cct, 20) << "lock image " << ictx << " exclusive=" << exclusive
			 << " cookie='" << cookie << "' tag='" << tag << "'"
			 << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    /**
     * If we wanted we could do something more intelligent, like local
     * checks that we think we will succeed. But for now, let's not
     * duplicate that code.
     */
    RWLock::RLocker locker(ictx->md_lock);
    r = rados::cls::lock::lock(&ictx->md_ctx, ictx->header_oid, RBD_LOCK_NAME,
			       exclusive ? LOCK_EXCLUSIVE : LOCK_SHARED,
			       cookie, tag, "", utime_t(), 0);
    if (r < 0)
      return r;
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return 0;
  }

  int unlock(ImageCtx *ictx, const string& cookie)
  {
    ldout(ictx->cct, 20) << "unlock image " << ictx
			 << " cookie='" << cookie << "'" << dendl;


    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    RWLock::RLocker locker(ictx->md_lock);
    r = rados::cls::lock::unlock(&ictx->md_ctx, ictx->header_oid,
				 RBD_LOCK_NAME, cookie);
    if (r < 0)
      return r;
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return 0;
  }

  int break_lock(ImageCtx *ictx, const string& client,
		 const string& cookie)
  {
    ldout(ictx->cct, 20) << "break_lock image " << ictx << " client='" << client
			 << "' cookie='" << cookie << "'" << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    entity_name_t lock_client;
    if (!lock_client.parse(client)) {
      lderr(ictx->cct) << "Unable to parse client '" << client
		       << "'" << dendl;
      return -EINVAL;
    }

    if (ictx->blacklist_on_break_lock) {
      typedef std::map<rados::cls::lock::locker_id_t,
		       rados::cls::lock::locker_info_t> Lockers;
      Lockers lockers;
      ClsLockType lock_type;
      std::string lock_tag;
      r = rados::cls::lock::get_lock_info(&ictx->md_ctx, ictx->header_oid,
                                          RBD_LOCK_NAME, &lockers, &lock_type,
                                          &lock_tag);
      if (r < 0) {
        lderr(ictx->cct) << "unable to retrieve lock info: " << cpp_strerror(r)
          	       << dendl;
        return r;
      }

      std::string client_address;
      for (Lockers::iterator it = lockers.begin();
           it != lockers.end(); ++it) {
        if (it->first.locker == lock_client) {
          client_address = stringify(it->second.addr);
          break;
        }
      }
      if (client_address.empty()) {
        return -ENOENT;
      }
      
      RWLock::RLocker locker(ictx->md_lock);
      librados::Rados rados(ictx->md_ctx);
      r = rados.blacklist_add(client_address,
			      ictx->blacklist_expire_seconds);
      if (r < 0) {
        lderr(ictx->cct) << "unable to blacklist client: " << cpp_strerror(r)
          	       << dendl;
        return r;
      }
    }

    r = rados::cls::lock::break_lock(&ictx->md_ctx, ictx->header_oid,
				     RBD_LOCK_NAME, cookie, lock_client);
    if (r < 0)
      return r;
    notify_change(ictx->md_ctx, ictx->header_oid, ictx);
    return 0;
  }

  void rbd_ctx_cb(completion_t cb, void *arg)
  {
    Context *ctx = reinterpret_cast<Context *>(arg);
    AioCompletion *comp = reinterpret_cast<AioCompletion *>(cb);
    ctx->complete(comp->get_return_value());
    comp->release();
  }

  int64_t read_iterate(ImageCtx *ictx, uint64_t off, uint64_t len,
		       int (*cb)(uint64_t, size_t, const char *, void *),
		       void *arg)
  {
    utime_t start_time, elapsed;

    ldout(ictx->cct, 20) << "read_iterate " << ictx << " off = " << off
			 << " len = " << len << dendl;

    int r = ictx_check(ictx);
    if (r < 0)
      return r;

    uint64_t mylen = len;
    ictx->snap_lock.get_read();
    r = clip_io(ictx, off, &mylen);
    ictx->snap_lock.put_read();
    if (r < 0)
      return r;

    int64_t total_read = 0;
    uint64_t period = ictx->get_stripe_period();
    uint64_t left = mylen;

    start_time = ceph_clock_now(ictx->cct);
    while (left > 0) {
      uint64_t period_off = off - (off % period);
      uint64_t read_len = min(period_off + period - off, left);

      bufferlist bl;

      Mutex mylock("IoCtxImpl::write::mylock");
      Cond cond;
      bool done;
      int ret;

      Context *ctx = new C_SafeCond(&mylock, &cond, &done, &ret);
      AioCompletion *c = aio_create_completion_internal(ctx, rbd_ctx_cb);
      aio_read(ictx, off, read_len, NULL, &bl, c, 0);

      mylock.Lock();
      while (!done)
	cond.Wait(mylock);
      mylock.Unlock();

      if (ret < 0)
	return ret;

      r = cb(total_read, ret, bl.c_str(), arg);
      if (r < 0)
	return r;

      total_read += ret;
      left -= ret;
      off += ret;
    }

    elapsed = ceph_clock_now(ictx->cct) - start_time;
    ictx->perfcounter->tinc(l_librbd_rd_latency, elapsed);
    ictx->perfcounter->inc(l_librbd_rd);
    ictx->perfcounter->inc(l_librbd_rd_bytes, mylen);
    return total_read;
  }

  int diff_iterate(ImageCtx *ictx, const char *fromsnapname, uint64_t off,
                   uint64_t len, bool include_parent, bool whole_object,
		   int (*cb)(uint64_t, size_t, int, void *), void *arg)
  {
    ldout(ictx->cct, 20) << "diff_iterate " << ictx << " off = " << off
			 << " len = " << len << dendl;

    // ensure previous writes are visible to listsnaps
    {
      RWLock::RLocker owner_locker(ictx->owner_lock);
      _flush(ictx);
    }

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    ictx->snap_lock.get_read();
    r = clip_io(ictx, off, &len);
    ictx->snap_lock.put_read();
    if (r < 0) {
      return r;
    }

    DiffIterate command(*ictx, fromsnapname, off, len, include_parent,
                        whole_object, cb, arg);
    r = command.execute();
    return r;
  }

  int simple_read_cb(uint64_t ofs, size_t len, const char *buf, void *arg)
  {
    char *dest_buf = (char *)arg;
    if (buf)
      memcpy(dest_buf + ofs, buf, len);
    else
      memset(dest_buf + ofs, 0, len);

    return 0;
  }

  ssize_t read(ImageCtx *ictx, uint64_t ofs, size_t len, char *buf, int op_flags)
  {
    ssize_t ret;
    ldout(ictx->cct, 20) << "read " << ictx << " off = " << ofs << " len = "
			 << len << dendl;

    vector<pair<uint64_t,uint64_t> > extents;
    extents.push_back(make_pair(ofs, len));
    ret = read(ictx, extents, buf, NULL, op_flags);
    if (ret < 0)
      return ret;

    return ret;
  }

  ssize_t read(ImageCtx *ictx, const vector<pair<uint64_t,uint64_t> >& image_extents,
		char *buf, bufferlist *pbl, int op_flags)
  {
    Mutex mylock("librbd::read::mylock");
    Cond cond;
    bool done;
    int ret;

    Context *ctx = new C_SafeCond(&mylock, &cond, &done, &ret);
    AioCompletion *c = aio_create_completion_internal(ctx, rbd_ctx_cb);
    aio_read(ictx, image_extents, buf, pbl, c, op_flags);

    mylock.Lock();
    while (!done)
      cond.Wait(mylock);
    mylock.Unlock();

    return ret;
  }

  ssize_t write(ImageCtx *ictx, uint64_t off, size_t len, const char *buf, int op_flags)
  {
    ldout(ictx->cct, 20) << "write " << ictx << " off = " << off << " len = "
			 << len << dendl;

    Mutex mylock("librbd::write::mylock");
    Cond cond;
    bool done;
    int ret;

    uint64_t mylen = len;
    ictx->snap_lock.get_read();
    int r = clip_io(ictx, off, &mylen);
    ictx->snap_lock.put_read();
    if (r < 0) {
      return r;
    }

    Context *ctx = new C_SafeCond(&mylock, &cond, &done, &ret);
    AioCompletion *c = aio_create_completion_internal(ctx, rbd_ctx_cb);
    aio_write(ictx, off, mylen, buf, c, op_flags);

    mylock.Lock();
    while (!done)
      cond.Wait(mylock);
    mylock.Unlock();

    if (ret < 0) {
      return ret;
    }

    return mylen;
  }

  int discard(ImageCtx *ictx, uint64_t off, uint64_t len)
  {
    ldout(ictx->cct, 20) << "discard " << ictx << " off = " << off << " len = "
			 << len << dendl;

    Mutex mylock("librbd::discard::mylock");
    Cond cond;
    bool done;
    int ret;

    uint64_t mylen = len;
    ictx->snap_lock.get_read();
    int r = clip_io(ictx, off, &mylen);
    ictx->snap_lock.put_read();
    if (r < 0) {
      return r;
    }

    Context *ctx = new C_SafeCond(&mylock, &cond, &done, &ret);
    AioCompletion *c = aio_create_completion_internal(ctx, rbd_ctx_cb);
    aio_discard(ictx, off, mylen, c);

    mylock.Lock();
    while (!done)
      cond.Wait(mylock);
    mylock.Unlock();

    if (ret < 0) {
      return ret;
    }

    return mylen;
  }

  ssize_t handle_sparse_read(CephContext *cct,
			     bufferlist data_bl,
			     uint64_t block_ofs,
			     const map<uint64_t, uint64_t> &data_map,
			     uint64_t buf_ofs,   // offset into buffer
			     size_t buf_len,     // length in buffer (not size of buffer!)
			     char *dest_buf)
  {
    uint64_t bl_ofs = 0;
    size_t buf_left = buf_len;

    for (map<uint64_t, uint64_t>::const_iterator iter = data_map.begin();
	 iter != data_map.end();
	 ++iter) {
      uint64_t extent_ofs = iter->first;
      size_t extent_len = iter->second;

      ldout(cct, 10) << "extent_ofs=" << extent_ofs
		     << " extent_len=" << extent_len << dendl;
      ldout(cct, 10) << "block_ofs=" << block_ofs << dendl;

      /* a hole? */
      if (extent_ofs > block_ofs) {
	uint64_t gap = extent_ofs - block_ofs;
	ldout(cct, 10) << "<1>zeroing " << buf_ofs << "~" << gap << dendl;
	memset(dest_buf + buf_ofs, 0, gap);

	buf_ofs += gap;
	buf_left -= gap;
	block_ofs = extent_ofs;
      } else if (extent_ofs < block_ofs) {
	assert(0 == "osd returned data prior to what we asked for");
	return -EIO;
      }

      if (bl_ofs + extent_len > (buf_ofs + buf_left)) {
	assert(0 == "osd returned more data than we asked for");
	return -EIO;
      }

      /* data */
      ldout(cct, 10) << "<2>copying " << buf_ofs << "~" << extent_len
		     << " from ofs=" << bl_ofs << dendl;
      memcpy(dest_buf + buf_ofs, data_bl.c_str() + bl_ofs, extent_len);

      bl_ofs += extent_len;
      buf_ofs += extent_len;
      assert(buf_left >= extent_len);
      buf_left -= extent_len;
      block_ofs += extent_len;
    }

    /* last hole */
    if (buf_left > 0) {
      ldout(cct, 10) << "<3>zeroing " << buf_ofs << "~" << buf_left << dendl;
      memset(dest_buf + buf_ofs, 0, buf_left);
    }

    return buf_len;
  }

  void rados_req_cb(rados_completion_t c, void *arg)
  {
    AioRequest *req = reinterpret_cast<AioRequest *>(arg);
    req->complete(rados_aio_get_return_value(c));
  }

  void rados_ctx_cb(rados_completion_t c, void *arg)
  {
    Context *comp = reinterpret_cast<Context *>(arg);
    comp->complete(rados_aio_get_return_value(c));
  }

  // validate extent against image size; clip to image size if necessary
  int clip_io(ImageCtx *ictx, uint64_t off, uint64_t *len)
  {
    assert(ictx->snap_lock.is_locked());
    uint64_t image_size = ictx->get_image_size(ictx->snap_id);
    bool snap_exists = ictx->snap_exists;

    if (!snap_exists)
      return -ENOENT;

    // special-case "len == 0" requests: always valid
    if (*len == 0)
      return 0;

    // can't start past end
    if (off >= image_size)
      return -EINVAL;

    // clip requests that extend past end to just end
    if ((off + *len) > image_size)
      *len = (size_t)(image_size - off);

    return 0;
  }

  void aio_flush(ImageCtx *ictx, AioCompletion *c)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "aio_flush " << ictx << " completion " << c <<  dendl;

    c->get();
    int r = ictx_check(ictx);
    if (r < 0) {
      c->fail(cct, r);
      return;
    }

    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->user_flushed();

    C_AioWrite *flush_ctx = new C_AioWrite(cct, c);
    c->add_request();
    ictx->flush_async_operations(flush_ctx);

    c->start_op(ictx, AIO_TYPE_FLUSH);
    C_AioWrite *req_comp = new C_AioWrite(cct, c);
    c->add_request();
    if (ictx->object_cacher) {
      ictx->flush_cache_aio(req_comp);
    } else {
      librados::AioCompletion *rados_completion =
	librados::Rados::aio_create_completion(req_comp, NULL, rados_ctx_cb);
      ictx->data_ctx.aio_flush_async(rados_completion);
      rados_completion->release();
    }
    c->finish_adding_requests(cct);
    c->put();
    ictx->perfcounter->inc(l_librbd_aio_flush);
  }

  int flush(ImageCtx *ictx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "flush " << ictx << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    ictx->user_flushed();
    {
      RWLock::RLocker owner_locker(ictx->owner_lock);
      r = _flush(ictx);
    }
    ictx->perfcounter->inc(l_librbd_flush);
    return r;
  }

  int _flush(ImageCtx *ictx)
  {
    assert(ictx->owner_lock.is_locked());
    CephContext *cct = ictx->cct;
    int r;
    // flush any outstanding writes
    if (ictx->object_cacher) {
      r = ictx->flush_cache();
    } else {
      r = ictx->data_ctx.aio_flush();
      ictx->flush_async_operations();
    }

    if (r)
      lderr(cct) << "_flush " << ictx << " r = " << r << dendl;

    return r;
  }

  int invalidate_cache(ImageCtx *ictx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "invalidate_cache " << ictx << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    ictx->flush_async_operations();

    RWLock::RLocker owner_locker(ictx->owner_lock);
    RWLock::WLocker md_locker(ictx->md_lock);
    r = ictx->invalidate_cache();
    return r;
  }

  void aio_write(ImageCtx *ictx, uint64_t off, size_t len, const char *buf,
		 AioCompletion *c, int op_flags)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "aio_write " << ictx << " off = " << off << " len = "
		   << len << " buf = " << (void*)buf << dendl;

    c->get();
    int r = ictx_check(ictx);
    if (r < 0) {
      c->fail(cct, r);
      return;
    }

    RWLock::RLocker owner_locker(ictx->owner_lock);
    RWLock::RLocker md_locker(ictx->md_lock);

    uint64_t clip_len = len;
    ::SnapContext snapc;
    {
      // prevent image size from changing between computing clip and recording
      // pending async operation
      RWLock::RLocker snap_locker(ictx->snap_lock);
      if (ictx->snap_id != CEPH_NOSNAP || ictx->read_only) {
        c->fail(cct, -EROFS);
        return;
      }

      r = clip_io(ictx, off, &clip_len);
      if (r < 0) {
        c->fail(cct, r);
        return;
      }

      snapc = ictx->snapc;
      c->start_op(ictx, AIO_TYPE_WRITE);
    }

    if (ictx->image_watcher->is_lock_supported() &&
	!ictx->image_watcher->is_lock_owner()) {
      c->put();
      ictx->image_watcher->request_lock(
	boost::bind(&librbd::aio_write, ictx, off, len, buf, _1, op_flags), c);
      return;
    }

    // map
    vector<ObjectExtent> extents;
    if (len > 0) {
      Striper::file_to_extents(ictx->cct, ictx->format_string,
			       &ictx->layout, off, clip_len, 0, extents);
    }

    for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); ++p) {
      ldout(cct, 20) << " oid " << p->oid << " " << p->offset << "~" << p->length
		     << " from " << p->buffer_extents << dendl;
      // assemble extent
      bufferlist bl;
      for (vector<pair<uint64_t,uint64_t> >::iterator q = p->buffer_extents.begin();
	   q != p->buffer_extents.end();
	   ++q) {
	bl.append(buf + q->first, q->second);
      }

      C_AioWrite *req_comp = new C_AioWrite(cct, c);
      if (ictx->object_cacher) {
	c->add_request();
	ictx->write_to_cache(p->oid, bl, p->length, p->offset, req_comp, op_flags);
      } else {
	AioWrite *req = new AioWrite(ictx, p->oid.name, p->objectno, p->offset,
				     bl, snapc, req_comp);
	c->add_request();

	req->set_op_flags(op_flags);
	req->send();
      }
    }

    c->finish_adding_requests(ictx->cct);
    c->put();

    ictx->perfcounter->inc(l_librbd_wr);
    ictx->perfcounter->inc(l_librbd_wr_bytes, clip_len);
  }

  int metadata_get(ImageCtx *ictx, const string &key, string *value)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "metadata_get " << ictx << " key=" << key << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    return cls_client::metadata_get(&ictx->md_ctx, ictx->header_oid, key, value);
  }

  int metadata_set(ImageCtx *ictx, const string &key, const string &value)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "metadata_set " << ictx << " key=" << key << " value=" << value << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    map<string, bufferlist> data;
    data[key].append(value);
    return cls_client::metadata_set(&ictx->md_ctx, ictx->header_oid, data);
  }

  int metadata_remove(ImageCtx *ictx, const string &key)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "metadata_remove " << ictx << " key=" << key << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    return cls_client::metadata_remove(&ictx->md_ctx, ictx->header_oid, key);
  }

  int metadata_list(ImageCtx *ictx, const string &start, uint64_t max, map<string, bufferlist> *pairs)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "metadata_list " << ictx << dendl;

    int r = ictx_check(ictx);
    if (r < 0) {
      return r;
    }

    return cls_client::metadata_list(&ictx->md_ctx, ictx->header_oid, start, max, pairs);
  }

  void aio_discard(ImageCtx *ictx, uint64_t off, uint64_t len, AioCompletion *c)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "aio_discard " << ictx << " off = " << off << " len = "
		   << len << dendl;

    c->get();
    int r = ictx_check(ictx);
    if (r < 0) {
      c->fail(cct, r);
      return;
    }

    RWLock::RLocker owner_locker(ictx->owner_lock);
    RWLock::RLocker md_locker(ictx->md_lock);

    uint64_t clip_len = len;
    ::SnapContext snapc;
    {
      // prevent image size from changing between computing clip and recording
      // pending async operation
      RWLock::RLocker snap_locker(ictx->snap_lock);
      if (ictx->snap_id != CEPH_NOSNAP || ictx->read_only) {
        c->fail(cct, -EROFS);
        return;
      }

      r = clip_io(ictx, off, &clip_len);
      if (r < 0) {
        c->fail(cct, r);
        return;
      }

      // TODO: check for snap
      snapc = ictx->snapc;
      c->start_op(ictx, AIO_TYPE_DISCARD);
    }

    if (ictx->image_watcher->is_lock_supported() &&
	!ictx->image_watcher->is_lock_owner()) {
      c->put();
      ictx->image_watcher->request_lock(
	boost::bind(&librbd::aio_discard, ictx, off, len, _1), c);
      return;
    }

    // map
    vector<ObjectExtent> extents;
    if (len > 0) {
      Striper::file_to_extents(ictx->cct, ictx->format_string,
			       &ictx->layout, off, clip_len, 0, extents);
    }

    for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); ++p) {
      ldout(cct, 20) << " oid " << p->oid << " " << p->offset << "~" << p->length
		     << " from " << p->buffer_extents << dendl;
      C_AioWrite *req_comp = new C_AioWrite(cct, c);
      AbstractWrite *req;
      c->add_request();

      if (p->length == ictx->layout.fl_object_size) {
	req = new AioRemove(ictx, p->oid.name, p->objectno, snapc, req_comp);
      } else if (p->offset + p->length == ictx->layout.fl_object_size) {
	req = new AioTruncate(ictx, p->oid.name, p->objectno, p->offset, snapc,
                              req_comp);
      } else {
	if(ictx->cct->_conf->rbd_skip_partial_discard) {
	  delete req_comp;
	  continue;
	}
	req = new AioZero(ictx, p->oid.name, p->objectno, p->offset, p->length,
			  snapc, req_comp);
      }

      req->send();
    }

    if (ictx->object_cacher) {
      Mutex::Locker l(ictx->cache_lock);
      ictx->object_cacher->discard_set(ictx->object_set, extents);
    }

    c->finish_adding_requests(ictx->cct);
    c->put();

    ictx->perfcounter->inc(l_librbd_discard);
    ictx->perfcounter->inc(l_librbd_discard_bytes, clip_len);
  }

  void rbd_req_cb(completion_t cb, void *arg)
  {
    AioRequest *req = reinterpret_cast<AioRequest *>(arg);
    AioCompletion *comp = reinterpret_cast<AioCompletion *>(cb);
    req->complete(comp->get_return_value());
  }

  void aio_read(ImageCtx *ictx, uint64_t off, size_t len,
	       char *buf, bufferlist *bl,
	       AioCompletion *c, int op_flags)
  {
    vector<pair<uint64_t,uint64_t> > image_extents(1);
    image_extents[0] = make_pair(off, len);
    aio_read(ictx, image_extents, buf, bl, c, op_flags);
  }

  struct C_RBD_Readahead : public Context {
    ImageCtx *ictx;
    object_t oid;
    uint64_t offset;
    uint64_t length;
    C_RBD_Readahead(ImageCtx *ictx, object_t oid, uint64_t offset, uint64_t length)
      : ictx(ictx), oid(oid), offset(offset), length(length) { }
    void finish(int r) {
      ldout(ictx->cct, 20) << "C_RBD_Readahead on " << oid << ": " << offset << "+" << length << dendl;
      ictx->readahead.dec_pending();
    }
  };

  static void readahead(ImageCtx *ictx,
			const vector<pair<uint64_t,uint64_t> >& image_extents)
  {
    uint64_t total_bytes = 0;
    for (vector<pair<uint64_t,uint64_t> >::const_iterator p = image_extents.begin();
	 p != image_extents.end();
	 ++p) {
      total_bytes += p->second;
    }
    ictx->md_lock.get_write();
    bool abort = ictx->readahead_disable_after_bytes != 0 &&
      ictx->total_bytes_read > ictx->readahead_disable_after_bytes;
    ictx->total_bytes_read += total_bytes;
    ictx->snap_lock.get_read();
    uint64_t image_size = ictx->get_image_size(ictx->snap_id);
    ictx->snap_lock.put_read();
    ictx->md_lock.put_write();
    if (abort) {
      return;
    }
    pair<uint64_t, uint64_t> readahead_extent = ictx->readahead.update(image_extents, image_size);
    uint64_t readahead_offset = readahead_extent.first;
    uint64_t readahead_length = readahead_extent.second;

    if (readahead_length > 0) {
      ldout(ictx->cct, 20) << "(readahead logical) " << readahead_offset << "~" << readahead_length << dendl;
      map<object_t,vector<ObjectExtent> > readahead_object_extents;
      Striper::file_to_extents(ictx->cct, ictx->format_string, &ictx->layout,
			       readahead_offset, readahead_length, 0, readahead_object_extents);
      for (map<object_t,vector<ObjectExtent> >::iterator p = readahead_object_extents.begin(); p != readahead_object_extents.end(); ++p) {
	for (vector<ObjectExtent>::iterator q = p->second.begin(); q != p->second.end(); ++q) {
	  ldout(ictx->cct, 20) << "(readahead) oid " << q->oid << " " << q->offset << "~" << q->length << dendl;

	  Context *req_comp = new C_RBD_Readahead(ictx, q->oid, q->offset, q->length);
	  ictx->readahead.inc_pending();
	  ictx->aio_read_from_cache(q->oid, q->objectno, NULL,
				    q->length, q->offset,
				    req_comp, 0);
	}
      }
      ictx->perfcounter->inc(l_librbd_readahead);
      ictx->perfcounter->inc(l_librbd_readahead_bytes, readahead_length);
    }
  }

  void aio_read(ImageCtx *ictx, const vector<pair<uint64_t,uint64_t> >& image_extents,
	        char *buf, bufferlist *pbl, AioCompletion *c, int op_flags)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "aio_read " << ictx << " completion " << c << " " << image_extents << dendl;

    c->get();
    int r = ictx_check(ictx);
    if (r < 0) {
      c->fail(cct, r);
      return;
    }

    RWLock::RLocker owner_locker(ictx->owner_lock);

    // readahead
    if (ictx->object_cacher && ictx->readahead_max_bytes > 0 &&
	!(op_flags & LIBRADOS_OP_FLAG_FADVISE_RANDOM)) {
      readahead(ictx, image_extents);
    }

    snap_t snap_id;
    map<object_t,vector<ObjectExtent> > object_extents;
    uint64_t buffer_ofs = 0;
    {
      // prevent image size from changing between computing clip and recording
      // pending async operation
      RWLock::RLocker snap_locker(ictx->snap_lock);
      snap_id = ictx->snap_id;

      // map
      for (vector<pair<uint64_t,uint64_t> >::const_iterator p =
             image_extents.begin();
	   p != image_extents.end(); ++p) {
        uint64_t len = p->second;
        r = clip_io(ictx, p->first, &len);
        if (r < 0) {
          c->fail(cct, r);
	  return;
        }
        if (len == 0) {
	  continue;
        }

        Striper::file_to_extents(cct, ictx->format_string, &ictx->layout,
			         p->first, len, 0, object_extents, buffer_ofs);
        buffer_ofs += len;
      }
      c->start_op(ictx, AIO_TYPE_READ);
    }

    c->read_buf = buf;
    c->read_buf_len = buffer_ofs;
    c->read_bl = pbl;

    for (map<object_t,vector<ObjectExtent> >::iterator p = object_extents.begin();
         p != object_extents.end(); ++p) {
      for (vector<ObjectExtent>::iterator q = p->second.begin();
           q != p->second.end(); ++q) {
	ldout(ictx->cct, 20) << " oid " << q->oid << " " << q->offset << "~"
                             << q->length << " from " << q->buffer_extents
                             << dendl;

	C_AioRead *req_comp = new C_AioRead(ictx->cct, c);
	AioRead *req = new AioRead(ictx, q->oid.name, q->objectno, q->offset,
                                   q->length, q->buffer_extents, snap_id, true,
                                   req_comp, op_flags);
	req_comp->set_req(req);
	c->add_request();

	if (ictx->object_cacher) {
	  C_CacheRead *cache_comp = new C_CacheRead(ictx, req);
	  ictx->aio_read_from_cache(q->oid, q->objectno, &req->data(),
				    q->length, q->offset,
				    cache_comp, op_flags);
	} else {
	  req->send();
	}
      }
    }

    c->finish_adding_requests(cct);
    c->put();

    ictx->perfcounter->inc(l_librbd_rd);
    ictx->perfcounter->inc(l_librbd_rd_bytes, buffer_ofs);
  }

  AioCompletion *aio_create_completion() {
    AioCompletion *c = new AioCompletion();
    return c;
  }

  AioCompletion *aio_create_completion(void *cb_arg, callback_t cb_complete) {
    AioCompletion *c = new AioCompletion();
    c->set_complete_cb(cb_arg, cb_complete);
    return c;
  }

  AioCompletion *aio_create_completion_internal(void *cb_arg,
						callback_t cb_complete) {
    AioCompletion *c = aio_create_completion(cb_arg, cb_complete);
    c->rbd_comp = c;
    return c;
  }
}
