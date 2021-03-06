// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

/* 
 * This is the top level monitor. It runs on each machine in the Monitor   
 * Cluster. The election of a leader for the paxos algorithm only happens 
 * once per machine via the elector. There is a separate paxos instance (state) 
 * kept for each of the system components: Object Store Device (OSD) Monitor, 
 * Placement Group (PG) Monitor, Metadata Server (MDS) Monitor, and Client Monitor.
 */

#ifndef CEPH_MONITOR_H
#define CEPH_MONITOR_H

#include "include/types.h"
#include "msg/Messenger.h"

#include "common/Timer.h"

#include "MonMap.h"
#include "Elector.h"
#include "Paxos.h"
#include "Session.h"

#include "osd/OSDMap.h"

#include "common/LogClient.h"

#include "auth/cephx/CephxKeyServer.h"
#include "auth/AuthSupported.h"

#include "perfglue/heap_profiler.h"

#include <memory>

enum {
  l_cluster_first = 555000,
  l_cluster_num_mon,
  l_cluster_num_mon_quorum,
  l_cluster_num_osd,
  l_cluster_num_osd_up,
  l_cluster_num_osd_in,
  l_cluster_osd_epoch,
  l_cluster_osd_kb,
  l_cluster_osd_kb_used,
  l_cluster_osd_kb_avail,
  l_cluster_num_pool,
  l_cluster_num_pg,
  l_cluster_num_pg_active_clean,
  l_cluster_num_pg_active,
  l_cluster_num_pg_peering,
  l_cluster_num_object,
  l_cluster_num_object_degraded,
  l_cluster_num_object_unfound,
  l_cluster_num_kb,
  l_cluster_num_mds_up,
  l_cluster_num_mds_in,
  l_cluster_num_mds_failed,
  l_cluster_mds_epoch,
  l_cluster_last,
};

class MonitorStore;

class PaxosService;

class PerfCounters;

class MMonGetMap;
class MMonGetVersion;
class MMonProbe;
class MMonObserve;
class MMonSubscribe;
class MAuthRotating;
class MRoute;
class MForward;

#define COMPAT_SET_LOC "feature_set"

class Monitor : public Dispatcher {
public:
  // me
  string name;
  int rank;
  Messenger *messenger;
  Mutex lock;
  SafeTimer timer;
  
  PerfCounters *logger, *cluster_logger;
  bool cluster_logger_registered;

  void register_cluster_logger();
  void unregister_cluster_logger();

  MonMap *monmap;

  LogClient clog;
  KeyServer key_server;

  AuthSupported auth_supported;

private:
  void new_tick();
  friend class C_Mon_Tick;

  // -- local storage --
public:
  MonitorStore *store;

  // -- monitor state --
private:
  enum {
    STATE_PROBING = 1,
    STATE_SLURPING,
    STATE_ELECTING,
    STATE_LEADER,
    STATE_PEON
  };
  int state;

public:
  static const char *get_state_name(int s) {
    switch (s) {
    case STATE_PROBING: return "probing";
    case STATE_SLURPING: return "slurping";
    case STATE_ELECTING: return "electing";
    case STATE_LEADER: return "leader";
    case STATE_PEON: return "peon";
    default: return "???";
    }
  }
  const char *get_state_name() {
    return get_state_name(state);
  }

  bool is_probing() const { return state == STATE_PROBING; }
  bool is_slurping() const { return state == STATE_SLURPING; }
  bool is_electing() const { return state == STATE_ELECTING; }
  bool is_leader() const { return state == STATE_LEADER; }
  bool is_peon() const { return state == STATE_PEON; }

  const utime_t &get_leader_since() const;

  // -- elector --
private:
  Elector elector;
  friend class Elector;
  
  int leader;            // current leader (to best of knowledge)
  set<int> quorum;       // current active set of monitors (if !starting)
  utime_t leader_since;  // when this monitor became the leader, if it is the leader
  utime_t exited_quorum; // time detected as not in quorum; 0 if in

  set<string> outside_quorum;
  entity_inst_t slurp_source;
  map<string,version_t> slurp_versions;

  list<Context*> waitfor_quorum;

  Context *probe_timeout_event;  // for probing and slurping states

  struct C_ProbeTimeout : public Context {
    Monitor *mon;
    C_ProbeTimeout(Monitor *m) : mon(m) {}
    void finish(int r) {
      mon->probe_timeout(r);
    }
  };

  void reset_probe_timeout();
  void cancel_probe_timeout();
  void probe_timeout(int r);

  void slurp();
  
public:
  epoch_t get_epoch();
  int get_leader() { return leader; }
  const set<int>& get_quorum() { return quorum; }

  void bootstrap();
  void reset();
  void start_election();
  void win_standalone_election();
  void win_election(epoch_t epoch, set<int>& q);         // end election (called by Elector)
  void lose_election(epoch_t epoch, set<int>& q, int l); // end election (called by Elector)
  void finish_election();

  void update_logger();

  // -- paxos --
  vector<Paxos*> paxos;
  vector<PaxosService*> paxos_service;

  Paxos *add_paxos(int type);
  Paxos *get_paxos_by_name(const string& name);

  class PGMonitor *pgmon() { return (class PGMonitor *)paxos_service[PAXOS_PGMAP]; }
  class MDSMonitor *mdsmon() { return (class MDSMonitor *)paxos_service[PAXOS_MDSMAP]; }
  class MonmapMonitor *monmon() { return (class MonmapMonitor *)paxos_service[PAXOS_MONMAP]; }
  class OSDMonitor *osdmon() { return (class OSDMonitor *)paxos_service[PAXOS_OSDMAP]; }
  class AuthMonitor *authmon() { return (class AuthMonitor *)paxos_service[PAXOS_AUTH]; }

  friend class Paxos;
  friend class OSDMonitor;
  friend class MDSMonitor;
  friend class MonmapMonitor;
  friend class PGMonitor;
  friend class LogMonitor;


  // -- sessions --
  MonSessionMap session_map;

  void check_subs();
  void check_sub(Subscription *sub);

  void send_latest_monmap(Connection *con);
  

  // messages
  void handle_get_version(MMonGetVersion *m);
  void handle_subscribe(MMonSubscribe *m);
  void handle_mon_get_map(MMonGetMap *m);
  bool _allowed_command(MonSession *s, const vector<std::string>& cmd);
  void handle_command(class MMonCommand *m);
  void handle_observe(MMonObserve *m);
  void handle_route(MRoute *m);

  void reply_command(MMonCommand *m, int rc, const string &rs, version_t version);
  void reply_command(MMonCommand *m, int rc, const string &rs, bufferlist& rdata, version_t version);

  void handle_probe(MMonProbe *m);
  void handle_probe_probe(MMonProbe *m);
  void handle_probe_reply(MMonProbe *m);
  void handle_probe_slurp(MMonProbe *m);
  void handle_probe_slurp_latest(MMonProbe *m);
  void handle_probe_data(MMonProbe *m);

  // request routing
  struct RoutedRequest {
    uint64_t tid;
    entity_inst_t client;
    bufferlist request_bl;
    MonSession *session;

    ~RoutedRequest() {
      if (session)
	session->put();
    }
  };
  uint64_t routed_request_tid;
  map<uint64_t, RoutedRequest*> routed_requests;
  
  void forward_request_leader(PaxosServiceMessage *req);
  void handle_forward(MForward *m);
  void try_send_message(Message *m, entity_inst_t to);
  void send_reply(PaxosServiceMessage *req, Message *reply);
  void resend_routed_requests();
  void remove_session(MonSession *s);

  void send_command(const entity_inst_t& inst,
		    const vector<string>& com, version_t version);

public:
  struct C_Command : public Context {
    Monitor *mon;
    MMonCommand *m;
    int rc;
    string rs;
    version_t version;
    C_Command(Monitor *_mm, MMonCommand *_m, int r, string& s, version_t v) :
      mon(_mm), m(_m), rc(r), rs(s), version(v){}
    void finish(int r) {
      mon->reply_command(m, rc, rs, version);
    }
  };

 private:
  class C_RetryMessage : public Context {
    Monitor *mon;
    Message *msg;
  public:
    C_RetryMessage(Monitor *m, Message *ms) : mon(m), msg(ms) {}
    void finish(int r) {
      mon->_ms_dispatch(msg);
    }
  };

  //ms_dispatch handles a lot of logic and we want to reuse it
  //on forwarded messages, so we create a non-locking version for this class
  bool _ms_dispatch(Message *m);
  bool ms_dispatch(Message *m) {
    lock.Lock();
    bool ret = _ms_dispatch(m);
    lock.Unlock();
    return ret;
  }
  //mon_caps is used for un-connected messages from monitors
  MonCaps * mon_caps;
  bool ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new);
  bool ms_verify_authorizer(Connection *con, int peer_type,
			    int protocol, bufferlist& authorizer_data, bufferlist& authorizer_reply,
			    bool& isvalid);
  bool ms_handle_reset(Connection *con);
  void ms_handle_remote_reset(Connection *con) {}

 public:
  Monitor(CephContext *cct_, string nm, MonitorStore *s, Messenger *m, MonMap *map);
  ~Monitor();

  void init();
  void shutdown();
  void tick();

  void stop_cluster();

  int mkfs(bufferlist& osdmapbl);

private:
  // don't allow copying
  Monitor(const Monitor& rhs);
  Monitor& operator=(const Monitor &rhs);
};

#define CEPH_MON_FEATURE_INCOMPAT_BASE CompatSet::Feature (1, "initial feature set (~v.18)")


#endif
