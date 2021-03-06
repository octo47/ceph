
#include <errno.h>
#include <stdlib.h>

#include <sstream>

#include "common/Clock.h"
#include "common/armor.h"
#include "common/mime.h"
#include "common/utf8.h"

#include "rgw_access.h"
#include "rgw_op.h"
#include "rgw_rest.h"
#include "rgw_acl.h"
#include "rgw_user.h"
#include "rgw_log.h"
#include "rgw_multi.h"

#define DOUT_SUBSYS rgw

using namespace std;
using ceph::crypto::MD5;

static string mp_ns = "multipart";
static string tmp_ns = "tmp";

class MultipartMetaFilter : public RGWAccessListFilter {
public:
  MultipartMetaFilter() {}
  bool filter(string& name, string& key) {
    int len = name.size();
    if (len < 6)
      return false;

    int pos = name.find(MP_META_SUFFIX, len - 5);
    if (pos <= 0)
      return false;

    pos = name.rfind('.', pos - 1);
    if (pos < 0)
      return false;

    key = name.substr(0, pos);

    return true;
  }
};

static MultipartMetaFilter mp_filter;

static int parse_range(const char *range, off_t& ofs, off_t& end, bool *partial_content)
{
  int r = -ERANGE;
  string s(range);
  string ofs_str;
  string end_str;

  *partial_content = false;

  int pos = s.find("bytes=");
  if (pos < 0) {
    pos = 0;
    while (isspace(s[pos]))
      pos++;
    int end = pos;
    while (isalpha(s[end]))
      end++;
    if (strncasecmp(s.c_str(), "bytes", end - pos) != 0)
      return 0;
    while (isspace(s[end]))
      end++;
    if (s[end] != '=')
      return 0;
    s = s.substr(end + 1);
  } else {
    s = s.substr(pos + 6); /* size of("bytes=")  */
  }
  pos = s.find('-');
  if (pos < 0)
    goto done;

  *partial_content = true;

  ofs_str = s.substr(0, pos);
  end_str = s.substr(pos + 1);
  if (end_str.length()) {
    end = atoll(end_str.c_str());
    if (end < 0)
      goto done;
  }

  if (ofs_str.length()) {
    ofs = atoll(ofs_str.c_str());
  } else { // RFC2616 suffix-byte-range-spec
    ofs = -end;
    end = -1;
  }

  dout(10) << "parse_range ofs=" << ofs << " end=" << end << dendl;

  if (end >= 0 && end < ofs)
    goto done;

  r = 0;
done:
  return r;
}

static void format_xattr(std::string &xattr)
{
  /* If the extended attribute is not valid UTF-8, we encode it using quoted-printable
   * encoding.
   */
  if ((check_utf8(xattr.c_str(), xattr.length()) != 0) ||
      (check_for_control_characters(xattr.c_str(), xattr.length()) != 0)) {
    static const char MIME_PREFIX_STR[] = "=?UTF-8?Q?";
    static const int MIME_PREFIX_LEN = sizeof(MIME_PREFIX_STR) - 1;
    static const char MIME_SUFFIX_STR[] = "?=";
    static const int MIME_SUFFIX_LEN = sizeof(MIME_SUFFIX_STR) - 1;
    int mlen = mime_encode_as_qp(xattr.c_str(), NULL, 0);
    char *mime = new char[MIME_PREFIX_LEN + mlen + MIME_SUFFIX_LEN + 1];
    strcpy(mime, MIME_PREFIX_STR);
    mime_encode_as_qp(xattr.c_str(), mime + MIME_PREFIX_LEN, mlen);
    strcpy(mime + MIME_PREFIX_LEN + (mlen - 1), MIME_SUFFIX_STR);
    xattr.assign(mime);
    delete [] mime;
    dout(10) << "format_xattr: formatted as '" << xattr << "'" << dendl;
  }
}

/**
 * Get the HTTP request metadata out of the req_state as a
 * map(<attr_name, attr_contents>, where attr_name is RGW_ATTR_PREFIX.HTTP_NAME)
 * s: The request state
 * attrs: will be filled up with attrs mapped as <attr_name, attr_contents>
 *
 */
static void get_request_metadata(struct req_state *s, map<string, bufferlist>& attrs)
{
  map<string, string>::iterator iter;
  for (iter = s->x_meta_map.begin(); iter != s->x_meta_map.end(); ++iter) {
    const string &name(iter->first);
    string &xattr(iter->second);
    dout(10) << "x>> " << name << ":" << xattr << dendl;
    format_xattr(xattr);
    string attr_name(RGW_ATTR_PREFIX);
    attr_name.append(name);
    map<string, bufferlist>::value_type v(attr_name, bufferlist());
    std::pair < map<string, bufferlist>::iterator, bool > rval(attrs.insert(v));
    bufferlist& bl(rval.first->second);
    bl.append(xattr.c_str(), xattr.size() + 1);
  }
}

/**
 * Get the AccessControlPolicy for an object off of disk.
 * policy: must point to a valid RGWACL, and will be filled upon return.
 * bucket: name of the bucket containing the object.
 * object: name of the object to get the ACL for.
 * Returns: 0 on success, -ERR# otherwise.
 */
static int get_policy_from_attr(void *ctx, RGWAccessControlPolicy *policy, rgw_obj& obj)
{
  bufferlist bl;
  int ret = 0;

  if (obj.bucket.name.size()) {
    ret = rgwstore->get_attr(ctx, obj, RGW_ATTR_ACL, bl);

    if (ret >= 0) {
      bufferlist::iterator iter = bl.begin();
      try {
        policy->decode(iter);
      } catch (buffer::error& err) {
        dout(0) << "ERROR: could not decode policy, caught buffer::error" << dendl;
        return -EIO;
      }
      if (g_conf->debug_rgw >= 15) {
        dout(15) << "Read AccessControlPolicy";
        policy->to_xml(*_dout);
        *_dout << dendl;
      }
    }
  }

  return ret;
}

static int get_obj_attrs(struct req_state *s, rgw_obj& obj, map<string, bufferlist>& attrs, uint64_t *obj_size)
{
  void *handle;
  int ret = rgwstore->prepare_get_obj(s->obj_ctx, obj, NULL, NULL, &attrs, NULL,
                                      NULL, NULL, NULL, NULL, NULL, obj_size, &handle, &s->err);
  rgwstore->finish_get_obj(&handle);
  return ret;
}

static int read_acls(struct req_state *s, RGWBucketInfo& bucket_info, RGWAccessControlPolicy *policy, rgw_bucket& bucket, string& object)
{
  string upload_id;
  url_decode(s->args.get("uploadId"), upload_id);
  string oid = object;
  rgw_obj obj;

  if (bucket_info.flags & BUCKET_SUSPENDED) {
    dout(0) << "NOTICE: bucket " << bucket_info.bucket.name << " is suspended" << dendl;
    return -ERR_USER_SUSPENDED;
  }

  if (!oid.empty() && !upload_id.empty()) {
    RGWMPObj mp(oid, upload_id);
    oid = mp.get_meta();
    obj.set_ns(mp_ns);
  }
  obj.init(bucket, oid, object);
  int ret = get_policy_from_attr(s->obj_ctx, policy, obj);
  if (ret == -ENOENT && object.size()) {
    /* object does not exist checking the bucket's ACL to make sure
       that we send a proper error code */
    RGWAccessControlPolicy bucket_policy;
    string no_object;
    rgw_obj no_obj(bucket, no_object);
    ret = get_policy_from_attr(s->obj_ctx, &bucket_policy, no_obj);
    if (ret < 0)
      return ret;

    if (!verify_permission(&bucket_policy, s->user.user_id, s->perm_mask, RGW_PERM_READ))
      ret = -EACCES;
    else
      ret = -ENOENT;
  } else if (ret == -ENOENT) {
      ret = -ERR_NO_SUCH_BUCKET;
  }

  return ret;
}

/**
 * Get the AccessControlPolicy for a bucket or object off of disk.
 * s: The req_state to draw information from.
 * only_bucket: If true, reads the bucket ACL rather than the object ACL.
 * Returns: 0 on success, -ERR# otherwise.
 */
static int read_acls(struct req_state *s, bool only_bucket, bool prefetch_data)
{
  int ret = 0;
  string obj_str;
  if (!s->acl) {
     s->acl = new RGWAccessControlPolicy;
     if (!s->acl)
       return -ENOMEM;
  }


  RGWBucketInfo bucket_info;
  if (s->bucket_name_str.size()) {
    ret = rgwstore->get_bucket_info(s->obj_ctx, s->bucket_name_str, bucket_info);
    if (ret < 0) {
      dout(0) << "NOTICE: couldn't get bucket from bucket_name (name=" << s->bucket_name_str << ")" << dendl;
      return ret;
    }
    s->bucket = bucket_info.bucket;
    s->bucket_owner = bucket_info.owner;
  }

  /* we're passed only_bucket = true when we specifically need the bucket's
     acls, that happens on write operations */
  if (!only_bucket) {
    obj_str = s->object_str;
    rgw_obj obj(s->bucket, obj_str);
    rgwstore->set_atomic(s->obj_ctx, obj);
    if (prefetch_data) {
      rgwstore->set_prefetch_data(s->obj_ctx, obj);
    }
  }

  ret = read_acls(s, bucket_info, s->acl, s->bucket, obj_str);

  return ret;
}

int RGWGetObj::verify_permission()
{
  obj.init(s->bucket, s->object_str);
  rgwstore->set_atomic(s->obj_ctx, obj);
  rgwstore->set_prefetch_data(s->obj_ctx, obj);

  if (!::verify_permission(s, RGW_PERM_READ))
    return -EACCES;

  return 0;
}

void RGWGetObj::execute()
{
  void *handle = NULL;
  utime_t start_time = s->time;

  perfcounter->inc(l_rgw_get);

  ret = get_params();
  if (ret < 0)
    goto done;

  ret = init_common();
  if (ret < 0)
    goto done;

  ret = rgwstore->prepare_get_obj(s->obj_ctx, obj, &ofs, &end, &attrs, mod_ptr,
                                  unmod_ptr, &lastmod, if_match, if_nomatch, &total_len, &s->obj_size, &handle, &s->err);
  if (ret < 0)
    goto done;

  start = ofs;

  if (!get_data || ofs > end)
    goto done;

  perfcounter->inc(l_rgw_get_b, end - ofs);

  while (ofs <= end) {
    data = NULL;
    ret = rgwstore->get_obj(s->obj_ctx, &handle, obj, &data, ofs, end);
    if (ret < 0) {
      goto done;
    }
    len = ret;
    ofs += len;
    ret = 0;

    perfcounter->finc(l_rgw_get_lat,
                     (ceph_clock_now(g_ceph_context) - start_time));
    send_response(handle);
    free(data);
    start_time = ceph_clock_now(g_ceph_context);
  }

  return;

done:
  send_response(handle);
  free(data);
  rgwstore->finish_get_obj(&handle);
}

int RGWGetObj::init_common()
{
  if (range_str) {
    int r = parse_range(range_str, ofs, end, &partial_content);
    if (r < 0)
      return r;
  }
  if (if_mod) {
    if (parse_time(if_mod, &mod_time) < 0)
      return -EINVAL;
    mod_ptr = &mod_time;
  }

  if (if_unmod) {
    if (parse_time(if_unmod, &unmod_time) < 0)
      return -EINVAL;
    unmod_ptr = &unmod_time;
  }

  return 0;
}

int RGWListBuckets::verify_permission()
{
  return 0;
}

void RGWListBuckets::execute()
{
  ret = get_params();
  if (ret < 0)
    goto done;

  ret = rgw_read_user_buckets(s->user.user_id, buckets, !!(s->prot_flags & RGW_REST_SWIFT));
  if (ret < 0) {
    /* hmm.. something wrong here.. the user was authenticated, so it
       should exist, just try to recreate */
    dout(10) << "WARNING: failed on rgw_get_user_buckets uid=" << s->user.user_id << dendl;

    /*

    on a second thought, this is probably a bug and we should fail

    rgw_put_user_buckets(s->user.user_id, buckets);
    ret = 0;

    */
  }

done:
  send_response();
}

int RGWStatAccount::verify_permission()
{
  return 0;
}

void RGWStatAccount::execute()
{
  RGWUserBuckets buckets;

  ret = rgw_read_user_buckets(s->user.user_id, buckets, true);
  if (ret < 0) {
    /* hmm.. something wrong here.. the user was authenticated, so it
       should exist, just try to recreate */
    dout(10) << "WARNING: failed on rgw_get_user_buckets uid=" << s->user.user_id << dendl;

    /*

    on a second thought, this is probably a bug and we should fail

    rgw_put_user_buckets(s->user.user_id, buckets);
    ret = 0;

    */
  } else {
    map<string, RGWBucketEnt>& m = buckets.get_buckets();
    map<string, RGWBucketEnt>::iterator iter;
    for (iter = m.begin(); iter != m.end(); ++iter) {
      RGWBucketEnt& bucket = iter->second;
      buckets_size += bucket.size;
      buckets_size_rounded += bucket.size_rounded;
      buckets_objcount += bucket.count;
    }
    buckets_count = m.size();
  }

  send_response();
}

int RGWStatBucket::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_READ))
    return -EACCES;

  return 0;
}

void RGWStatBucket::execute()
{
  RGWUserBuckets buckets;
  bucket.bucket = s->bucket;
  buckets.add(bucket);
  map<string, RGWBucketEnt>& m = buckets.get_buckets();
  ret = rgwstore->update_containers_stats(m);
  if (!ret)
    ret = -EEXIST;
  if (ret > 0) {
    ret = 0;
    map<string, RGWBucketEnt>::iterator iter = m.find(bucket.bucket.name);
    if (iter != m.end()) {
      bucket = iter->second;
    } else {
      ret = -EINVAL;
    }
  }

  send_response();
}

int RGWListBucket::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_READ))
    return -EACCES;

  return 0;
}

int RGWListBucket::parse_max_keys()
{
  if (!max_keys.empty()) {
    char *endptr;
    max = strtol(max_keys.c_str(), &endptr, 10);
    if (endptr) {
      while (*endptr && isspace(*endptr)) // ignore white space
        endptr++;
      if (*endptr) {
        return -EINVAL;
      }
    }
  } else {
    max = default_max;
  }

  return 0;
}

void RGWListBucket::execute()
{
  string no_ns;

  ret = get_params();
  if (ret < 0)
    goto done;

  ret = rgwstore->list_objects(s->bucket, max, prefix, delimiter, marker, objs, common_prefixes,
                               !!(s->prot_flags & RGW_REST_SWIFT), no_ns, &is_truncated, NULL);

done:
  send_response();
}

int RGWCreateBucket::verify_permission()
{
  if (!rgw_user_is_authenticated(s->user))
    return -EACCES;

  return 0;
}

void RGWCreateBucket::execute()
{
  RGWAccessControlPolicy policy, old_policy;
  map<string, bufferlist> attrs;
  bufferlist aclbl;
  bool existed;
  bool pol_ret;

  rgw_obj obj(rgw_root_bucket, s->bucket_name_str);
  s->bucket_owner = s->user.user_id;

  int r = get_policy_from_attr(s->obj_ctx, &old_policy, obj);
  if (r >= 0)  {
    if (old_policy.get_owner().get_id().compare(s->user.user_id) != 0) {
      ret = -EEXIST;
      goto done;
    }
  }
  pol_ret = policy.create_canned(s->user.user_id, s->user.display_name, s->canned_acl);
  if (!pol_ret) {
    ret = -EINVAL;
    goto done;
  }
  policy.encode(aclbl);

  attrs[RGW_ATTR_ACL] = aclbl;

  s->bucket.name = s->bucket_name_str;
  ret = rgwstore->create_bucket(s->user.user_id, s->bucket, attrs, false,
                                true, s->user.auid);
  /* continue if EEXIST and create_bucket will fail below.  this way we can recover
   * from a partial create by retrying it. */
  dout(20) << "rgw_create_bucket returned ret=" << ret << " bucket=" << s->bucket << dendl;

  if (ret && ret != -EEXIST)   
    goto done;

  existed = (ret == -EEXIST);

  ret = rgw_add_bucket(s->user.user_id, s->bucket);
  if (ret && !existed && ret != -EEXIST)   /* if it exists (or previously existed), don't remove it! */
    rgw_remove_user_bucket_info(s->user.user_id, s->bucket);

  if (ret == -EEXIST)
    ret = -ERR_BUCKET_EXISTS;

done:
  send_response();
}

int RGWDeleteBucket::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

void RGWDeleteBucket::execute()
{
  ret = -EINVAL;

  if (s->bucket_name) {
    ret = rgwstore->delete_bucket(s->bucket);

    if (ret == 0) {
      ret = rgw_remove_user_bucket_info(s->user.user_id, s->bucket);
      if (ret < 0) {
        dout(0) << "WARNING: failed to remove bucket: ret=" << ret << dendl;
      }

      string oid;
      rgw_obj obj(s->bucket, oid);
      RGWIntentEvent intent = DEL_POOL;
      int r = rgw_log_intent(s, obj, intent);
      if (r < 0) {
        dout(0) << "WARNING: failed to log intent for bucket removal bucket=" << s->bucket << dendl;
      }
    }
  }

  send_response();
}

struct put_obj_aio_info {
  void *handle;
};

int RGWPutObj::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

class RGWPutObjProcessor_Plain : public RGWPutObjProcessor
{
  bufferlist data;
  rgw_obj obj;
  off_t ofs;

protected:
  int prepare(struct req_state *s);
  int handle_data(bufferlist& bl, off_t ofs, void **phandle);
  int throttle_data(void *handle) { return 0; }
  int complete(string& etag, map<string, bufferlist>& attrs);

public:
  RGWPutObjProcessor_Plain() : ofs(0) {}
};

int RGWPutObjProcessor_Plain::prepare(struct req_state *s)
{
  RGWPutObjProcessor::prepare(s);

  obj.init(s->bucket, s->object_str);

  return 0;
};

int RGWPutObjProcessor_Plain::handle_data(bufferlist& bl, off_t _ofs, void **phandle)
{
  if (ofs != _ofs)
    return -EINVAL;

  data.append(bl);
  ofs += bl.length();

  return 0;
}

int RGWPutObjProcessor_Plain::complete(string& etag, map<string, bufferlist>& attrs)
{
  int r = rgwstore->put_obj_meta(s->obj_ctx, obj, data.length(), NULL, attrs,
                                 RGW_OBJ_CATEGORY_MAIN, false, NULL, &data);
  return r;
}


class RGWPutObjProcessor_Aio : public RGWPutObjProcessor
{
  list<struct put_obj_aio_info> pending;
  size_t max_chunks;

  struct put_obj_aio_info pop_pending();
  int wait_pending_front();
  bool pending_has_completed();
  int drain_pending();

protected:
  rgw_obj obj;

  int handle_data(bufferlist& bl, off_t ofs, void **phandle);
  int throttle_data(void *handle);

  RGWPutObjProcessor_Aio() : max_chunks(RGW_MAX_PENDING_CHUNKS) {}
  virtual ~RGWPutObjProcessor_Aio() {
    drain_pending();
  }
};

int RGWPutObjProcessor_Aio::handle_data(bufferlist& bl, off_t ofs, void **phandle)
{
  // For the first call pass -1 as the offset to
  // do a write_full.
  int r = rgwstore->aio_put_obj_data(s->obj_ctx, obj,
                                     bl,
                                     ((ofs == 0) ? -1 : ofs),
                                     false, phandle);

  return r;
}

struct put_obj_aio_info RGWPutObjProcessor_Aio::pop_pending()
{
  struct put_obj_aio_info info;
  info = pending.front();
  pending.pop_front();
  return info;
}

int RGWPutObjProcessor_Aio::wait_pending_front()
{
  struct put_obj_aio_info info = pop_pending();
  int ret = rgwstore->aio_wait(info.handle);
  return ret;
}

bool RGWPutObjProcessor_Aio::pending_has_completed()
{
  if (pending.size() == 0)
    return false;

  struct put_obj_aio_info& info = pending.front();
  return rgwstore->aio_completed(info.handle);
}

int RGWPutObjProcessor_Aio::drain_pending()
{
  int ret = 0;
  while (!pending.empty()) {
    int r = wait_pending_front();
    if (r < 0)
      ret = r;
  }
  return ret;
}

int RGWPutObjProcessor_Aio::throttle_data(void *handle)
{
  struct put_obj_aio_info info;
  info.handle = handle;
  pending.push_back(info);
  size_t orig_size = pending.size();
  while (pending_has_completed()) {
    int r = wait_pending_front();
    if (r < 0)
      return r;
  }

  /* resize window in case messages are draining too fast */
  if (orig_size - pending.size() >= max_chunks)
  max_chunks++;

  if (pending.size() > max_chunks) {
    int r = wait_pending_front();
    if (r < 0)
      return r;
  }
  return 0;
}

class RGWPutObjProcessor_Atomic : public RGWPutObjProcessor_Aio
{
  bool remove_temp_obj;
protected:
  int prepare(struct req_state *s);
  int complete(string& etag, map<string, bufferlist>& attrs);

public:
  ~RGWPutObjProcessor_Atomic();
  RGWPutObjProcessor_Atomic() : remove_temp_obj(false) {}
  int handle_data(bufferlist& bl, off_t ofs, void **phandle) {
    int r = RGWPutObjProcessor_Aio::handle_data(bl, ofs, phandle);
    if (r >= 0) {
      remove_temp_obj = true;
    }
    return r;
  }
};

int RGWPutObjProcessor_Atomic::prepare(struct req_state *s)
{
  RGWPutObjProcessor::prepare(s);

  string oid = s->object_str;
  obj.set_ns(tmp_ns);

  char buf[33];
  gen_rand_alphanumeric(buf, sizeof(buf) - 1);
  oid.append("_");
  oid.append(buf);
  obj.init(s->bucket, oid, s->object_str);

  return 0;
}

int RGWPutObjProcessor_Atomic::complete(string& etag, map<string, bufferlist>& attrs)
{
  rgw_obj dst_obj(s->bucket, s->object_str);
  rgwstore->set_atomic(s->obj_ctx, dst_obj);
  int r = rgwstore->clone_obj(s->obj_ctx, dst_obj, 0, obj, 0, s->obj_size, NULL, attrs, RGW_OBJ_CATEGORY_MAIN);

  return r;
}
RGWPutObjProcessor_Atomic::~RGWPutObjProcessor_Atomic()
{
  if (remove_temp_obj)
    rgwstore->delete_obj(NULL, obj, NULL);
}

class RGWPutObjProcessor_Multipart : public RGWPutObjProcessor_Aio
{
  string part_num;
  RGWMPObj mp;
protected:
  int prepare(struct req_state *s);
  int complete(string& etag, map<string, bufferlist>& attrs);

public:
  RGWPutObjProcessor_Multipart() {}
};

int RGWPutObjProcessor_Multipart::prepare(struct req_state *s)
{
  RGWPutObjProcessor::prepare(s);

  string oid = s->object_str;
  string upload_id;
  url_decode(s->args.get("uploadId"), upload_id);
  mp.init(oid, upload_id);

  url_decode(s->args.get("partNumber"), part_num);
  if (part_num.empty()) {
    return -EINVAL;
  }
  oid = mp.get_part(part_num);

  obj.set_ns(mp_ns);
  obj.init(s->bucket, oid, s->object_str);
  return 0;
}

int RGWPutObjProcessor_Multipart::complete(string& etag, map<string, bufferlist>& attrs)
{
  int r = rgwstore->put_obj_meta(s->obj_ctx, obj, s->obj_size, NULL, attrs, RGW_OBJ_CATEGORY_MAIN, false, NULL, NULL);
  if (r < 0)
    return r;

  bufferlist bl;
  RGWUploadPartInfo info;
  string p = "part.";
  p.append(part_num);
  info.num = atoi(part_num.c_str());
  info.etag = etag;
  info.size = s->obj_size;
  info.modified = ceph_clock_now(g_ceph_context);
  ::encode(info, bl);

  string multipart_meta_obj = mp.get_meta();

  rgw_obj meta_obj(s->bucket, multipart_meta_obj, s->object_str, mp_ns);

  r = rgwstore->tmap_set(meta_obj, p, bl);

  return r;
}


RGWPutObjProcessor *RGWPutObj::select_processor()
{
  RGWPutObjProcessor *processor;

  bool multipart = s->args.exists("uploadId");

  if (!multipart) {
    if (s->content_length <= RGW_MAX_CHUNK_SIZE && !chunked_upload)
      processor = new RGWPutObjProcessor_Plain();
    else
      processor = new RGWPutObjProcessor_Atomic();
  } else {
    processor = new RGWPutObjProcessor_Multipart();
  }

  return processor;
}

void RGWPutObj::dispose_processor(RGWPutObjProcessor *processor)
{
  delete processor;
}

void RGWPutObj::execute()
{
  rgw_obj obj;
  RGWAccessControlPolicy policy;
  RGWPutObjProcessor *processor = NULL;
  char supplied_md5_bin[CEPH_CRYPTO_MD5_DIGESTSIZE + 1];
  char supplied_md5[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 1];
  char calc_md5[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 1];
  unsigned char m[CEPH_CRYPTO_MD5_DIGESTSIZE];
  MD5 hash;
  bufferlist bl, aclbl;
  map<string, bufferlist> attrs;
  int len;


  perfcounter->inc(l_rgw_put);
  ret = -EINVAL;
  if (!s->object) {
    goto done;
  }

  ret = get_params();
  if (ret < 0)
    goto done;

  ret = policy.create_canned(s->user.user_id, s->user.display_name, s->canned_acl);
  if (!ret) {
     ret = -EINVAL;
     goto done;
  }

  if (supplied_md5_b64) {
    dout(15) << "supplied_md5_b64=" << supplied_md5_b64 << dendl;
    ret = ceph_unarmor(supplied_md5_bin, &supplied_md5_bin[CEPH_CRYPTO_MD5_DIGESTSIZE + 1],
                       supplied_md5_b64, supplied_md5_b64 + strlen(supplied_md5_b64));
    dout(15) << "ceph_armor ret=" << ret << dendl;
    if (ret != CEPH_CRYPTO_MD5_DIGESTSIZE) {
      ret = -ERR_INVALID_DIGEST;
      goto done;
    }

    buf_to_hex((const unsigned char *)supplied_md5_bin, CEPH_CRYPTO_MD5_DIGESTSIZE, supplied_md5);
    dout(15) << "supplied_md5=" << supplied_md5 << dendl;
  }

  if (supplied_etag) {
    strncpy(supplied_md5, supplied_etag, sizeof(supplied_md5));
  }

  processor = select_processor();

  ret = processor->prepare(s);
  if (ret < 0)
    goto done;

  do {
    bufferlist data;
    len = get_data(data);
    if (len < 0) {
      ret = len;
      goto done;
    }
    if (!len)
      break;

    void *handle;
    ret = processor->handle_data(data, ofs, &handle);
    if (ret < 0)
      goto done;

    hash.Update((unsigned char *)data.c_str(), len);

    ret = processor->throttle_data(handle);
    if (ret < 0)
      goto done;

    ofs += len;
  } while (len > 0);

  // was this really needed? processor->complete() will synch
  // drain_pending(pending);

  if (!chunked_upload && (uint64_t)ofs != s->content_length) {
    ret = -ERR_REQUEST_TIMEOUT;
    goto done;
  }
  s->obj_size = ofs;
  perfcounter->inc(l_rgw_put_b, s->obj_size);

  hash.Final(m);

  buf_to_hex(m, CEPH_CRYPTO_MD5_DIGESTSIZE, calc_md5);

  if (supplied_md5_b64 && strcmp(calc_md5, supplied_md5)) {
     ret = -ERR_BAD_DIGEST;
     goto done;
  }
  policy.encode(aclbl);

  etag = calc_md5;

  if (supplied_etag && etag.compare(supplied_etag) != 0) {
    ret = -ERR_UNPROCESSABLE_ENTITY;
    goto done;
  }
  bl.append(etag.c_str(), etag.size() + 1);
  attrs[RGW_ATTR_ETAG] = bl;
  attrs[RGW_ATTR_ACL] = aclbl;

  if (s->content_type) {
    bl.clear();
    bl.append(s->content_type, strlen(s->content_type) + 1);
    attrs[RGW_ATTR_CONTENT_TYPE] = bl;
  }

  get_request_metadata(s, attrs);

  ret = processor->complete(etag, attrs);
done:
  dispose_processor(processor);
  perfcounter->finc(l_rgw_put_lat,
                   (ceph_clock_now(g_ceph_context) - s->time));
  send_response();
  return;
}

int RGWPutObjMetadata::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

void RGWPutObjMetadata::execute()
{
  ret = -EINVAL;

  const char *meta_prefix = RGW_ATTR_META_PREFIX;
  int meta_prefix_len = sizeof(RGW_ATTR_META_PREFIX) - 1;
  map<string, bufferlist> attrs, orig_attrs, rmattrs;
  map<string, bufferlist>::iterator iter;
  get_request_metadata(s, attrs);

  rgw_obj obj(s->bucket, s->object_str);

  rgwstore->set_atomic(s->obj_ctx, obj);

  uint64_t obj_size;

  /* check if obj exists, read orig attrs */
  ret = get_obj_attrs(s, obj, orig_attrs, &obj_size);
  if (ret < 0)
    goto done;

  /* only remove meta attrs */
  for (iter = orig_attrs.begin(); iter != orig_attrs.end(); ++iter) {
    const string& name = iter->first;
    if (name.compare(0, meta_prefix_len, meta_prefix) == 0) {
      rmattrs[name] = iter->second;
    } else if (attrs.find(name) == attrs.end()) {
      attrs[name] = iter->second;
    }
  }

  ret = rgwstore->put_obj_meta(s->obj_ctx, obj, obj_size, NULL, attrs, RGW_OBJ_CATEGORY_MAIN, false, &rmattrs, NULL);

done:
  send_response();
}

int RGWDeleteObj::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

void RGWDeleteObj::execute()
{
  ret = -EINVAL;
  rgw_obj obj(s->bucket, s->object_str);
  if (s->object) {
    rgwstore->set_atomic(s->obj_ctx, obj);
    ret = rgwstore->delete_obj(s->obj_ctx, obj);
  }

  send_response();
}

bool RGWCopyObj::parse_copy_location(const char *src, string& bucket_name, string& object)
{
  string url_src(src);
  string dec_src;

  url_decode(url_src, dec_src);
  src = dec_src.c_str();

  dout(15) << "decoded obj=" << src << dendl;

  if (*src == '/') ++src;

  string str(src);

  int pos = str.find("/");
  if (pos <= 0)
    return false;

  bucket_name = str.substr(0, pos);
  object = str.substr(pos + 1);

  if (object.size() == 0)
    return false;

  return true;
}

int RGWCopyObj::verify_permission()
{
  string empty_str;
  RGWAccessControlPolicy src_policy;
  ret = get_params();
  if (ret < 0)
    return ret;

  RGWBucketInfo src_bucket_info, dest_bucket_info;

  /* get buckets info (source and dest) */

  ret = rgwstore->get_bucket_info(s->obj_ctx, src_bucket_name, src_bucket_info);
  if (ret < 0)
    return ret;

  src_bucket = src_bucket_info.bucket;

  if (src_bucket_name.compare(dest_bucket_name) == 0) {
    dest_bucket_info = src_bucket_info;
  } else {
    ret = rgwstore->get_bucket_info(s->obj_ctx, dest_bucket_name, dest_bucket_info);
    if (ret < 0)
      return ret;
  }

  dest_bucket = dest_bucket_info.bucket;

  /* check source object permissions */
  ret = read_acls(s, src_bucket_info, &src_policy, src_bucket, src_object);
  if (ret < 0)
    return ret;

  if (!::verify_permission(&src_policy, s->user.user_id, s->perm_mask, RGW_PERM_READ))
    return -EACCES;

  RGWAccessControlPolicy dest_bucket_policy;

  /* check dest bucket permissions */
  ret = read_acls(s, dest_bucket_info, &dest_bucket_policy, dest_bucket, empty_str);
  if (ret < 0)
    return ret;

  if (!::verify_permission(&dest_bucket_policy, s->user.user_id, s->perm_mask, RGW_PERM_WRITE))
    return -EACCES;

  /* build a polict for the target object */
  RGWAccessControlPolicy dest_policy;

  ret = dest_policy.create_canned(s->user.user_id, s->user.display_name, s->canned_acl);
  if (!ret)
     return -EINVAL;

  dest_policy.encode(aclbl);

  return 0;
}


int RGWCopyObj::init_common()
{
  if (if_mod) {
    if (parse_time(if_mod, &mod_time) < 0) {
      ret = -EINVAL;
      return ret;
    }
    mod_ptr = &mod_time;
  }

  if (if_unmod) {
    if (parse_time(if_unmod, &unmod_time) < 0) {
      ret = -EINVAL;
      return ret;
    }
    unmod_ptr = &unmod_time;
  }

  attrs[RGW_ATTR_ACL] = aclbl;
  get_request_metadata(s, attrs);

  return 0;
}

void RGWCopyObj::execute()
{
  rgw_obj src_obj, dst_obj;

  if (init_common() < 0)
    goto done;

  src_obj.init(src_bucket, src_object);
  dst_obj.init(dest_bucket, dest_object);
  rgwstore->set_atomic(s->obj_ctx, src_obj);
  rgwstore->set_atomic(s->obj_ctx, dst_obj);

  ret = rgwstore->copy_obj(s->obj_ctx,
                        dst_obj,
                        src_obj,
                        &mtime,
                        mod_ptr,
                        unmod_ptr,
                        if_match,
                        if_nomatch,
                        attrs, RGW_OBJ_CATEGORY_MAIN, &s->err);

done:
  send_response();
}

int RGWGetACLs::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_READ_ACP))
    return -EACCES;

  return 0;
}

void RGWGetACLs::execute()
{
  ret = read_acls(s, false, false);

  if (ret < 0) {
    send_response();
    return;
  }

  stringstream ss;
  s->acl->to_xml(ss);
  acls = ss.str(); 
  send_response();
}

static int rebuild_policy(ACLOwner *owner, RGWAccessControlPolicy& src, RGWAccessControlPolicy& dest)
{
  if (!owner)
    return -EINVAL;

  ACLOwner *requested_owner = (ACLOwner *)src.find_first("Owner");
  if (requested_owner && requested_owner->get_id().compare(owner->get_id()) != 0) {
    return -EPERM;
  }

  RGWUserInfo owner_info;
  if (rgw_get_user_info_by_uid(owner->get_id(), owner_info) < 0) {
    dout(10) << "owner info does not exist" << dendl;
    return -EINVAL;
  }
  ACLOwner& dest_owner = dest.get_owner();
  dest_owner.set_id(owner->get_id());
  dest_owner.set_name(owner_info.display_name);

  dout(20) << "owner id=" << owner->get_id() << dendl;
  dout(20) << "dest owner id=" << dest.get_owner().get_id() << dendl;

  RGWAccessControlList& src_acl = src.get_acl();
  RGWAccessControlList& acl = dest.get_acl();

  XMLObjIter iter = src_acl.find("Grant");
  ACLGrant *src_grant = (ACLGrant *)iter.get_next();
  while (src_grant) {
    ACLGranteeType& type = src_grant->get_type();
    ACLGrant new_grant;
    bool grant_ok = false;
    string uid;
    RGWUserInfo grant_user;
    switch (type.get_type()) {
    case ACL_TYPE_EMAIL_USER:
      {
        string email = src_grant->get_id();
        dout(10) << "grant user email=" << email << dendl;
        if (rgw_get_user_info_by_email(email, grant_user) < 0) {
          dout(10) << "grant user email not found or other error" << dendl;
          return -ERR_UNRESOLVABLE_EMAIL;
        }
        uid = grant_user.user_id;
      }
    case ACL_TYPE_CANON_USER:
      {
        if (type.get_type() == ACL_TYPE_CANON_USER)
          uid = src_grant->get_id();
    
        if (grant_user.user_id.empty() && rgw_get_user_info_by_uid(uid, grant_user) < 0) {
          dout(10) << "grant user does not exist:" << uid << dendl;
          return -EINVAL;
        } else {
          ACLPermission& perm = src_grant->get_permission();
          new_grant.set_canon(uid, grant_user.display_name, perm.get_permissions());
          grant_ok = true;
          dout(10) << "new grant: " << new_grant.get_id() << ":" << grant_user.display_name << dendl;
        }
      }
      break;
    case ACL_TYPE_GROUP:
      {
        string group = src_grant->get_id();
        if (group.compare(RGW_URI_ALL_USERS) == 0 ||
            group.compare(RGW_URI_AUTH_USERS) == 0) {
          new_grant = *src_grant;
          grant_ok = true;
          dout(10) << "new grant: " << new_grant.get_id() << dendl;
        } else {
          dout(10) << "grant group does not exist:" << group << dendl;
          return -EINVAL;
        }
      }
    default:
      break;
    }
    if (grant_ok) {
      acl.add_grant(&new_grant);
    }
    src_grant = (ACLGrant *)iter.get_next();
  }

  return 0; 
}

int RGWPutACLs::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE_ACP))
    return -EACCES;

  return 0;
}

void RGWPutACLs::execute()
{
  bufferlist bl;

  RGWAccessControlPolicy *policy = NULL;
  RGWACLXMLParser parser;
  RGWAccessControlPolicy new_policy;
  stringstream ss;
  char *orig_data = data;
  char *new_data = NULL;
  ACLOwner owner;
  rgw_obj obj;

  ret = 0;

  if (!parser.init()) {
    ret = -EINVAL;
    goto done;
  }

  if (!s->acl) {
     s->acl = new RGWAccessControlPolicy;
     if (!s->acl) {
       ret = -ENOMEM;
       goto done;
     }
     owner.set_id(s->user.user_id);
     owner.set_name(s->user.display_name);
  } else {
     owner = s->acl->get_owner();
  }

  if (get_params() < 0)
    goto done;

  dout(15) << "read len=" << len << " data=" << (data ? data : "") << dendl;

  if (!s->canned_acl.empty() && len) {
    ret = -EINVAL;
    goto done;
  }
  if (!s->canned_acl.empty()) {
    RGWAccessControlPolicy canned_policy;
    bool r = canned_policy.create_canned(owner.get_id(), owner.get_display_name(), s->canned_acl);
    if (!r) {
      ret = -EINVAL;
      goto done;
    }
    canned_policy.to_xml(ss);
    new_data = strdup(ss.str().c_str());
    data = new_data;
    len = ss.str().size();
  }


  if (!parser.parse(data, len, 1)) {
    ret = -EACCES;
    goto done;
  }
  policy = (RGWAccessControlPolicy *)parser.find_first("AccessControlPolicy");
  if (!policy) {
    ret = -EINVAL;
    goto done;
  }

  if (g_conf->debug_rgw >= 15) {
    dout(15) << "Old AccessControlPolicy";
    policy->to_xml(*_dout);
    *_dout << dendl;
  }

  ret = rebuild_policy(&owner, *policy, new_policy);
  if (ret < 0)
    goto done;

  if (g_conf->debug_rgw >= 15) {
    dout(15) << "New AccessControlPolicy:";
    new_policy.to_xml(*_dout);
    *_dout << dendl;
  }

  new_policy.encode(bl);
  obj.init(s->bucket, s->object_str);
  rgwstore->set_atomic(s->obj_ctx, obj);
  ret = rgwstore->set_attr(s->obj_ctx, obj, RGW_ATTR_ACL, bl);

done:
  free(orig_data);
  free(new_data);

  send_response();
}

int RGWInitMultipart::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

void RGWInitMultipart::execute()
{
  bufferlist bl;
  bufferlist aclbl;
  RGWAccessControlPolicy policy;
  map<string, bufferlist> attrs;
  rgw_obj obj;

  if (get_params() < 0)
    goto done;
  ret = -EINVAL;
  if (!s->object)
    goto done;

  ret = policy.create_canned(s->user.user_id, s->user.display_name, s->canned_acl);
  if (!ret) {
     ret = -EINVAL;
     goto done;
  }

  policy.encode(aclbl);

  attrs[RGW_ATTR_ACL] = aclbl;

  if (s->content_type) {
    bl.append(s->content_type, strlen(s->content_type) + 1);
    attrs[RGW_ATTR_CONTENT_TYPE] = bl;
  }

  get_request_metadata(s, attrs);

  do {
    char buf[33];
    gen_rand_alphanumeric(buf, sizeof(buf) - 1);
    upload_id = buf;

    string tmp_obj_name;
    RGWMPObj mp(s->object_str, upload_id);
    tmp_obj_name = mp.get_meta();

    obj.init(s->bucket, tmp_obj_name, s->object_str, mp_ns);
    // the meta object will be indexed with 0 size, we c
    ret = rgwstore->put_obj_meta(s->obj_ctx, obj, 0, NULL, attrs, RGW_OBJ_CATEGORY_MULTIMETA, true, NULL, NULL);
  } while (ret == -EEXIST);
done:
  send_response();
}

static int get_multiparts_info(struct req_state *s, string& meta_oid, map<uint32_t, RGWUploadPartInfo>& parts,
                               RGWAccessControlPolicy& policy, map<string, bufferlist>& attrs)
{
  map<string, bufferlist> parts_map;
  map<string, bufferlist>::iterator iter;
  bufferlist header;

  rgw_obj obj(s->bucket, meta_oid, s->object_str, mp_ns);

  int ret = get_obj_attrs(s, obj, attrs, NULL);
  if (ret < 0)
    return ret;

  ret = rgwstore->tmap_get(obj, header, parts_map);
  if (ret < 0)
    return ret;

  for (iter = attrs.begin(); iter != attrs.end(); ++iter) {
    string name = iter->first;
    if (name.compare(RGW_ATTR_ACL) == 0) {
      bufferlist& bl = iter->second;
      bufferlist::iterator bli = bl.begin();
      try {
        ::decode(policy, bli);
      } catch (buffer::error& err) {
        dout(0) << "ERROR: could not decode policy, caught buffer::error" << dendl;
        return -EIO;
      }
      break;
    }
  }


  for (iter = parts_map.begin(); iter != parts_map.end(); ++iter) {
    bufferlist& bl = iter->second;
    bufferlist::iterator bli = bl.begin();
    RGWUploadPartInfo info;
    try {
      ::decode(info, bli);
    } catch (buffer::error& err) {
      dout(0) << "ERROR: could not decode policy, caught buffer::error" << dendl;
    }
    parts[info.num] = info;
  }
  return 0;
}

int RGWCompleteMultipart::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

void RGWCompleteMultipart::execute()
{
  RGWMultiCompleteUpload *parts;
  map<int, string>::iterator iter;
  RGWMultiXMLParser parser;
  string meta_oid;
  map<uint32_t, RGWUploadPartInfo> obj_parts;
  map<uint32_t, RGWUploadPartInfo>::iterator obj_iter;
  RGWAccessControlPolicy policy;
  map<string, bufferlist> attrs;
  off_t ofs = 0;
  MD5 hash;
  char final_etag[CEPH_CRYPTO_MD5_DIGESTSIZE];
  char final_etag_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 16];
  bufferlist etag_bl;
  rgw_obj meta_obj;
  rgw_obj target_obj;
  RGWMPObj mp;
  vector<RGWCloneRangeInfo> ranges;


  ret = get_params();
  if (ret < 0)
    goto done;

  if (!data) {
    ret = -EINVAL;
    goto done;
  }

  if (!parser.init()) {
    ret = -EINVAL;
    goto done;
  }

  if (!parser.parse(data, len, 1)) {
    ret = -EINVAL;
    goto done;
  }

  parts = (RGWMultiCompleteUpload *)parser.find_first("CompleteMultipartUpload");
  if (!parts) {
    ret = -EINVAL;
    goto done;
  }

  mp.init(s->object_str, upload_id);
  meta_oid = mp.get_meta();

  ret = get_multiparts_info(s, meta_oid, obj_parts, policy, attrs);
  if (ret == -ENOENT)
    ret = -ERR_NO_SUCH_UPLOAD;
  if (parts->parts.size() != obj_parts.size())
    ret = -ERR_INVALID_PART;
  if (ret < 0)
    goto done;

  for (iter = parts->parts.begin(), obj_iter = obj_parts.begin();
       iter != parts->parts.end() && obj_iter != obj_parts.end();
       ++iter, ++obj_iter) {
    char etag[CEPH_CRYPTO_MD5_DIGESTSIZE];
    if (iter->first != (int)obj_iter->first) {
      dout(0) << "NOTICE: parts num mismatch: next requested: " << iter->first << " next uploaded: " << obj_iter->first << dendl;
      ret = -ERR_INVALID_PART;
      goto done;
    }
    if (iter->second.compare(obj_iter->second.etag) != 0) {
      dout(0) << "NOTICE: etag mismatch: part: " << iter->first << " etag: " << iter->second << dendl;
      ret = -ERR_INVALID_PART;
      goto done;
    }

    hex_to_buf(obj_iter->second.etag.c_str(), etag, CEPH_CRYPTO_MD5_DIGESTSIZE);
    hash.Update((const byte *)etag, sizeof(etag));
  }
  hash.Final((byte *)final_etag);

  buf_to_hex((unsigned char *)final_etag, sizeof(final_etag), final_etag_str);
  snprintf(&final_etag_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2],  sizeof(final_etag_str) - CEPH_CRYPTO_MD5_DIGESTSIZE * 2,
           "-%lld", (long long)parts->parts.size());
  dout(10) << "calculated etag: " << final_etag_str << dendl;

  etag_bl.append(final_etag_str, strlen(final_etag_str) + 1);

  attrs[RGW_ATTR_ETAG] = etag_bl;

  target_obj.init(s->bucket, s->object_str);
  rgwstore->set_atomic(s->obj_ctx, target_obj);
  ret = rgwstore->put_obj_meta(s->obj_ctx, target_obj, 0, NULL, attrs, RGW_OBJ_CATEGORY_MAIN, false, NULL, NULL);
  if (ret < 0)
    goto done;
  
  for (obj_iter = obj_parts.begin(); obj_iter != obj_parts.end(); ++obj_iter) {
    string oid = mp.get_part(obj_iter->second.num);
    rgw_obj src_obj(s->bucket, oid, s->object_str, mp_ns);

    RGWCloneRangeInfo range;
    range.src = src_obj;
    range.src_ofs = 0;
    range.dst_ofs = ofs;
    range.len = obj_iter->second.size;
    ranges.push_back(range);

    ofs += obj_iter->second.size;
  }
  ret = rgwstore->clone_objs(s->obj_ctx, target_obj, ranges, attrs, RGW_OBJ_CATEGORY_MAIN, NULL, true, false);
  if (ret < 0)
    goto done;

  // now erase all parts
  for (obj_iter = obj_parts.begin(); obj_iter != obj_parts.end(); ++obj_iter) {
    string oid = mp.get_part(obj_iter->second.num);
    rgw_obj obj(s->bucket, oid, s->object_str, mp_ns);
    rgwstore->delete_obj(s->obj_ctx, obj);
  }
  // and also remove the metadata obj
  meta_obj.init(s->bucket, meta_oid, s->object_str, mp_ns);
  rgwstore->delete_obj(s->obj_ctx, meta_obj);

done:
  send_response();
}

int RGWAbortMultipart::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_WRITE))
    return -EACCES;

  return 0;
}

void RGWAbortMultipart::execute()
{
  ret = -EINVAL;
  string upload_id;
  string meta_oid;
  string prefix;
  url_decode(s->args.get("uploadId"), upload_id);
  map<uint32_t, RGWUploadPartInfo> obj_parts;
  map<uint32_t, RGWUploadPartInfo>::iterator obj_iter;
  RGWAccessControlPolicy policy;
  map<string, bufferlist> attrs;
  rgw_obj meta_obj;
  RGWMPObj mp;

  if (upload_id.empty() || s->object_str.empty())
    goto done;

  mp.init(s->object_str, upload_id); 
  meta_oid = mp.get_meta();

  ret = get_multiparts_info(s, meta_oid, obj_parts, policy, attrs);
  if (ret < 0)
    goto done;

  for (obj_iter = obj_parts.begin(); obj_iter != obj_parts.end(); ++obj_iter) {
    string oid = mp.get_part(obj_iter->second.num);
    rgw_obj obj(s->bucket, oid, s->object_str, mp_ns);
    ret = rgwstore->delete_obj(s->obj_ctx, obj);
    if (ret < 0 && ret != -ENOENT)
      goto done;
  }
  // and also remove the metadata obj
  meta_obj.init(s->bucket, meta_oid, s->object_str, mp_ns);
  ret = rgwstore->delete_obj(s->obj_ctx, meta_obj);
  if (ret == -ENOENT) {
    ret = -ERR_NO_SUCH_BUCKET;
  }
done:

  send_response();
}

int RGWListMultipart::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_READ))
    return -EACCES;

  return 0;
}

void RGWListMultipart::execute()
{
  map<string, bufferlist> xattrs;
  string meta_oid;
  RGWMPObj mp;

  ret = get_params();
  if (ret < 0)
    goto done;

  mp.init(s->object_str, upload_id);
  meta_oid = mp.get_meta();

  ret = get_multiparts_info(s, meta_oid, parts, policy, xattrs);

done:
  send_response();
}

int RGWListBucketMultiparts::verify_permission()
{
  if (!::verify_permission(s, RGW_PERM_READ))
    return -EACCES;

  return 0;
}

void RGWListBucketMultiparts::execute()
{
  vector<RGWObjEnt> objs;
  string marker_meta;

  ret = get_params();
  if (ret < 0)
    goto done;

  if (s->prot_flags & RGW_REST_SWIFT) {
    string path_args;
    url_decode(s->args.get("path"), path_args);
    if (!path_args.empty()) {
      if (!delimiter.empty() || !prefix.empty()) {
        ret = -EINVAL;
        goto done;
      }
      url_decode(path_args, prefix);
      delimiter="/";
    }
  }
  marker_meta = marker.get_meta();
  ret = rgwstore->list_objects(s->bucket, max_uploads, prefix, delimiter, marker_meta, objs, common_prefixes,
                               !!(s->prot_flags & RGW_REST_SWIFT), mp_ns, &is_truncated, &mp_filter);
  if (objs.size()) {
    vector<RGWObjEnt>::iterator iter;
    RGWMultipartUploadEntry entry;
    for (iter = objs.begin(); iter != objs.end(); ++iter) {
      string name = iter->name;
      if (!entry.mp.from_meta(name))
        continue;
      entry.obj = *iter;
      uploads.push_back(entry);
    }
    next_marker = entry;
  }
done:
  send_response();
}

int RGWHandler::init(struct req_state *_s, FCGX_Request *fcgx)
{
  s = _s;

  if (g_conf->debug_rgw >= 20) {
    char *p;
    for (int i=0; (p = fcgx->envp[i]); ++i) {
      dout(20) << p << dendl;
    }
  }
  return 0;
}

int RGWHandler::do_read_permissions(RGWOp *op, bool only_bucket)
{
  int ret = read_acls(s, only_bucket, op->prefetch_data());

  if (ret < 0) {
    dout(10) << "read_permissions on " << s->bucket << ":" <<s->object_str << " only_bucket=" << only_bucket << " ret=" << ret << dendl;
    if (ret == -ENODATA)
      ret = -EACCES;
  }

  return ret;
}

