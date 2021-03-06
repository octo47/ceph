// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_AUTHSUPPORTED_H
#define CEPH_AUTHSUPPORTED_H

#include "include/inttypes.h"
#include <set>

class CephContext;

class AuthSupported {
  std::set<__u32> auth_supported;
public:
  AuthSupported(CephContext *cct);

  bool is_supported_auth(int auth_type);
  int pick(const std::set<__u32>& supported);

  const std::set<__u32>& get_supported_set() const {
    return auth_supported;
  }
};


#endif
