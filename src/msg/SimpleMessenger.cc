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

#include "SimpleMessenger.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/uio.h>
#include <limits.h>
#include <sys/user.h>

#include "common/config.h"
#include "global/global_init.h"

#include "messages/MGenericMessage.h"

#include <netdb.h>

#include <iostream>
#include <fstream>

#include "common/Timer.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "include/page.h"

#include "include/compat.h"

#define DOUT_SUBSYS ms
#undef dout_prefix
#define dout_prefix _prefix(_dout, msgr)
static ostream& _prefix(std::ostream *_dout, SimpleMessenger *msgr) {
  return *_dout << "-- " << msgr->ms_addr << " ";
}


#include "tcp.cc"

/********************************************
 * Accepter
 */

int SimpleMessenger::Accepter::bind(uint64_t nonce, entity_addr_t &bind_addr, int avoid_port1, int avoid_port2)
{
  const md_config_t *conf = msgr->cct->_conf;
  // bind to a socket
  ldout(msgr->cct,10) << "accepter.bind" << dendl;
  
  int family;
  switch (bind_addr.get_family()) {
  case AF_INET:
  case AF_INET6:
    family = bind_addr.get_family();
    break;

  default:
    // bind_addr is empty
    family = conf->ms_bind_ipv6 ? AF_INET6 : AF_INET;
  }

  /* socket creation */
  listen_sd = ::socket(family, SOCK_STREAM, 0);
  if (listen_sd < 0) {
    char buf[80];
    ldout(msgr->cct,0) << "accepter.bind unable to create socket: "
	    << strerror_r(errno, buf, sizeof(buf)) << dendl;
    cerr << "accepter.bind unable to create socket: "
	 << strerror_r(errno, buf, sizeof(buf)) << std::endl;
    return -errno;
  }

  // use whatever user specified (if anything)
  entity_addr_t listen_addr = bind_addr;
  listen_addr.set_family(family);

  /* bind to port */
  int rc = -1;
  if (listen_addr.get_port()) {
    // specific port

    // reuse addr+port when possible
    int on = 1;
    ::setsockopt(listen_sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    rc = ::bind(listen_sd, (struct sockaddr *) &listen_addr.ss_addr(), listen_addr.addr_size());
    if (rc < 0) {
      char buf[80];
      ldout(msgr->cct,0) << "accepter.bind unable to bind to " << bind_addr.ss_addr()
	      << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      cerr << "accepter.bind unable to bind to " << bind_addr.ss_addr()
	   << ": " << strerror_r(errno, buf, sizeof(buf)) << std::endl;
      return -errno;
    }
  } else {
    // try a range of ports
    for (int port = CEPH_PORT_START; port <= CEPH_PORT_LAST; port++) {
      if (port == avoid_port1 || port == avoid_port2)
	continue;
      listen_addr.set_port(port);
      rc = ::bind(listen_sd, (struct sockaddr *) &listen_addr.ss_addr(), listen_addr.addr_size());
      if (rc == 0)
	break;
    }
    if (rc < 0) {
      char buf[80];
      ldout(msgr->cct,0) << "accepter.bind unable to bind to " << bind_addr.ss_addr()
	      << " on any port in range " << CEPH_PORT_START << "-" << CEPH_PORT_LAST
	      << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      cerr << "accepter.bind unable to bind to " << bind_addr.ss_addr()
	   << " on any port in range " << CEPH_PORT_START << "-" << CEPH_PORT_LAST
	   << ": " << strerror_r(errno, buf, sizeof(buf)) << std::endl;
      return -errno;
    }
    ldout(msgr->cct,10) << "accepter.bind bound on random port " << listen_addr << dendl;
  }

  // what port did we get?
  socklen_t llen = sizeof(listen_addr.ss_addr());
  getsockname(listen_sd, (sockaddr*)&listen_addr.ss_addr(), &llen);
  
  ldout(msgr->cct,10) << "accepter.bind bound to " << listen_addr << dendl;

  // listen!
  rc = ::listen(listen_sd, 128);
  if (rc < 0) {
    char buf[80];
    ldout(msgr->cct,0) << "accepter.bind unable to listen on " << listen_addr
	    << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    cerr << "accepter.bind unable to listen on " << listen_addr
	 << ": " << strerror_r(errno, buf, sizeof(buf)) << std::endl;
    return -errno;
  }
  
  msgr->ms_addr = bind_addr;
  if (msgr->ms_addr != entity_addr_t())
    msgr->need_addr = false;
  else 
    msgr->need_addr = true;

  if (msgr->ms_addr.get_port() == 0) {
    msgr->ms_addr = listen_addr;
    msgr->ms_addr.nonce = nonce;
  }

  msgr->init_local_pipe();

  ldout(msgr->cct,1) << "accepter.bind ms_addr is " << msgr->ms_addr << " need_addr=" << msgr->need_addr << dendl;
  msgr->did_bind = true;
  return 0;
}

int SimpleMessenger::Accepter::rebind(int avoid_port)
{
  ldout(msgr->cct,1) << "accepter.rebind avoid " << avoid_port << dendl;
  assert(msgr->did_bind);
  
  stop();

  entity_addr_t addr = msgr->ms_addr;
  int old_port = addr.get_port();
  addr.set_port(0);

  ldout(msgr->cct,10) << " will try " << addr << dendl;
  int r = bind(addr.get_nonce(), addr, old_port, avoid_port);
  if (r == 0)
    start();
  return r;
}

int SimpleMessenger::Accepter::start()
{
  ldout(msgr->cct,1) << "accepter.start" << dendl;

  // start thread
  create();

  return 0;
}

void *SimpleMessenger::Accepter::entry()
{
  const md_config_t *conf = msgr->cct->_conf;
  ldout(msgr->cct,10) << "accepter starting" << dendl;
  
  int errors = 0;

  char buf[80];

  struct pollfd pfd;
  pfd.fd = listen_sd;
  pfd.events = POLLIN | POLLERR | POLLNVAL | POLLHUP;
  while (!done) {
    ldout(msgr->cct,20) << "accepter calling poll" << dendl;
    int r = poll(&pfd, 1, -1);
    if (r < 0)
      break;
    ldout(msgr->cct,20) << "accepter poll got " << r << dendl;

    if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))
      break;

    ldout(msgr->cct,10) << "pfd.revents=" << pfd.revents << dendl;
    if (done) break;

    // accept
    entity_addr_t addr;
    socklen_t slen = sizeof(addr.ss_addr());
    int sd = ::accept(listen_sd, (sockaddr*)&addr.ss_addr(), &slen);
    if (sd >= 0) {
      errors = 0;
      ldout(msgr->cct,10) << "accepted incoming on sd " << sd << dendl;
      
      // disable Nagle algorithm?
      if (conf->ms_tcp_nodelay) {
	int flag = 1;
	int r = ::setsockopt(sd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
	if (r < 0)
	  ldout(msgr->cct,0) << "accepter could't set TCP_NODELAY: " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      }
      
      msgr->lock.Lock();

      if (!msgr->destination_stopped) {
	Pipe *p = new Pipe(msgr, Pipe::STATE_ACCEPTING);
	p->sd = sd;
	p->pipe_lock.Lock();
	p->start_reader();
	p->pipe_lock.Unlock();
	msgr->pipes.insert(p);
      }
      msgr->lock.Unlock();
    } else {
      ldout(msgr->cct,0) << "accepter no incoming connection?  sd = " << sd
	      << " errno " << errno << " " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      if (++errors > 4)
	break;
    }
  }

  ldout(msgr->cct,20) << "accepter closing" << dendl;
  // don't close socket, in case we start up again?  blech.
  if (listen_sd >= 0) {
    ::close(listen_sd);
    listen_sd = -1;
  }
  ldout(msgr->cct,10) << "accepter stopping" << dendl;
  return 0;
}

void SimpleMessenger::Accepter::stop()
{
  done = true;
  ldout(msgr->cct,10) << "stop accepter" << dendl;
  if (listen_sd >= 0) {
    ::shutdown(listen_sd, SHUT_RDWR);
    ::close(listen_sd);
    listen_sd = -1;
  }
  join();
  done = false;
  msgr->did_bind = false;
}






//**********************************

/*
 * This function delivers incoming messages to the Messenger.
 * Pipes with messages are kept in queues; when beginning a message
 * delivery the highest-priority queue is selected, the pipe from the
 * front of the queue is removed, and its message read. If the pipe
 * has remaining messages at that priority level, it is re-placed on to the
 * end of the queue. If the queue is empty; it's removed.
 * The message is then delivered and the process starts again.
 */
void SimpleMessenger::dispatch_entry()
{
  dispatch_queue.lock.Lock();
  while (!dispatch_queue.stop) {
    while (!dispatch_queue.queued_pipes.empty() && !dispatch_queue.stop) {
      //get highest-priority pipe
      map<int, xlist<Pipe *>* >::reverse_iterator high_iter =
	dispatch_queue.queued_pipes.rbegin();
      int priority = high_iter->first;
      xlist<Pipe *> *pipe_list = high_iter->second;

      Pipe *pipe = pipe_list->front();
      //move pipe to back of line -- or just take off if no more messages
      pipe->pipe_lock.Lock();
      list<Message *>& m_queue = pipe->in_q[priority];
      Message *m = m_queue.front();
      m_queue.pop_front();

      if (m_queue.empty()) {
	pipe_list->pop_front();  // pipe is done
	if (pipe_list->empty()) {
	  delete pipe_list;
	  dispatch_queue.queued_pipes.erase(priority);
	}
      } else {
	pipe_list->push_back(pipe->queue_items[priority]);  // move to end of list
      }
      ldout(cct,20) << "dispatch_entry pipe " << pipe << " dequeued " << m << dendl;
      dispatch_queue.lock.Unlock(); //done with the pipe queue for a while

      pipe->in_qlen--;
      dispatch_queue.qlen.dec();

      pipe->pipe_lock.Unlock(); // done with the pipe's message queue now
       {
	if ((long)m == DispatchQueue::D_BAD_REMOTE_RESET) {
	  dispatch_queue.lock.Lock();
	  Connection *con = dispatch_queue.remote_reset_q.front();
	  dispatch_queue.remote_reset_q.pop_front();
	  dispatch_queue.lock.Unlock();
	  ms_deliver_handle_remote_reset(con);
	  con->put();
	} else if ((long)m == DispatchQueue::D_CONNECT) {
	  dispatch_queue.lock.Lock();
	  Connection *con = dispatch_queue.connect_q.front();
	  dispatch_queue.connect_q.pop_front();
	  dispatch_queue.lock.Unlock();
	  ms_deliver_handle_connect(con);
	  con->put();
	} else if ((long)m == DispatchQueue::D_BAD_RESET) {
	  dispatch_queue.lock.Lock();
	  Connection *con = dispatch_queue.reset_q.front();
	  dispatch_queue.reset_q.pop_front();
	  dispatch_queue.lock.Unlock();
	  ms_deliver_handle_reset(con);
	  con->put();
	} else {
	  uint64_t msize = m->get_dispatch_throttle_size();
	  m->set_dispatch_throttle_size(0);  // clear it out, in case we requeue this message.

	  ldout(cct,1) << "<== " << m->get_source_inst()
		  << " " << m->get_seq()
		  << " ==== " << *m
		  << " ==== " << m->get_payload().length() << "+" << m->get_middle().length()
		  << "+" << m->get_data().length()
		  << " (" << m->get_footer().front_crc << " " << m->get_footer().middle_crc
		  << " " << m->get_footer().data_crc << ")"
		  << " " << m << " con " << m->get_connection()
		  << dendl;
	  ms_deliver_dispatch(m);

	  dispatch_throttle_release(msize);

	  ldout(cct,20) << "done calling dispatch on " << m << dendl;
	}
      }
      dispatch_queue.lock.Lock();
    }
    if (!dispatch_queue.stop)
      dispatch_queue.cond.Wait(dispatch_queue.lock); //wait for something to be put on queue
  }
  dispatch_queue.lock.Unlock();

  //tell everything else it's time to stop
  lock.Lock();
  destination_stopped = true;
  wait_cond.Signal();
  lock.Unlock();
}

void SimpleMessenger::ready()
{
  ldout(cct,10) << "ready " << get_myaddr() << dendl;
  assert(!dispatch_thread.is_started());
  dispatch_thread.create();
}


int SimpleMessenger::shutdown()
{
  ldout(cct,10) << "shutdown " << get_myaddr() << dendl;

  // stop my dispatch thread
  if (dispatch_thread.am_self()) {
    ldout(cct,10) << "shutdown i am dispatch, setting stop flag" << dendl;
    dispatch_queue.stop = true;
  } else {
    ldout(cct,10) << "shutdown i am not dispatch, setting stop flag and joining thread." << dendl;
    dispatch_queue.lock.Lock();
    dispatch_queue.stop = true;
    dispatch_queue.cond.Signal();
    dispatch_queue.lock.Unlock();
  }
  return 0;
}

void SimpleMessenger::suicide()
{
  ldout(cct,10) << "suicide " << get_myaddr() << dendl;
  shutdown();
  // hmm, or exit(0)?
}

void SimpleMessenger::prepare_dest(const entity_inst_t& inst)
{
  lock.Lock();
  {
    if (rank_pipe.count(inst.addr) == 0)
      connect_rank(inst.addr, inst.name.type());
  }
  lock.Unlock();
}

int SimpleMessenger::send_message(Message *m, const entity_inst_t& dest)
{
  // set envelope
  m->get_header().src = get_myname();

  if (!m->get_priority()) m->set_priority(get_default_send_priority());
 
  ldout(cct,1) << "--> " << dest.name << " " << dest.addr
          << " -- " << *m
    	  << " -- ?+" << m->get_data().length()
	  << " " << m 
	  << dendl;

  submit_message(m, dest.addr, dest.name.type(), false);
  return 0;
}

int SimpleMessenger::send_message(Message *m, Connection *con)
{
  //set envelope
  m->get_header().src = get_myname();

  if (!m->get_priority()) m->set_priority(get_default_send_priority());

  SimpleMessenger::Pipe *pipe = (SimpleMessenger::Pipe *)con->get_pipe();
  if (pipe) {
    ldout(cct,1) << "--> " << con->get_peer_addr() << " -- " << *m
            << " -- ?+" << m->get_data().length()
            << " " << m << " con " << con
            << dendl;

    submit_message(m, pipe);
    pipe->put();
  } else {
    ldout(cct,0) << "send_message dropped message " << *m << " because of no pipe on con " << con
	    << dendl;
    // else we raced with reaper()
    m->put();
  }
  return 0;
}

int SimpleMessenger::lazy_send_message(Message *m, const entity_inst_t& dest)
{
  // set envelope
  m->get_header().src = get_myname();

  if (!m->get_priority()) m->set_priority(get_default_send_priority());
 
  ldout(cct,1) << "lazy "
	  << " --> " << dest.name << " " << dest.addr
          << " -- " << *m
    	  << " -- ?+" << m->get_data().length()
          << " " << m
          << dendl;

  submit_message(m, dest.addr, dest.name.type(), true);
  return 0;
}

entity_addr_t SimpleMessenger::get_myaddr()
{
  entity_addr_t a = ms_addr;
  return a;  
}

/**
 * If ms_addr doesn't have an IP set, this function
 * will fill it in from the passed addr. Otherwise it does nothing and returns.
 */
void SimpleMessenger::set_ip(entity_addr_t &addr)
{
  if (ms_addr.is_blank_ip()) {
    int port = ms_addr.get_port();
    ms_addr.addr = addr.addr;
    ms_addr.set_port(port);
  }
}



/**************************************
 * Pipe
 */

#undef dout_prefix
#define dout_prefix _pipe_prefix(_dout)
ostream& SimpleMessenger::Pipe::_pipe_prefix(std::ostream *_dout) {
  return *_dout << "-- " << msgr->ms_addr << " >> " << peer_addr << " pipe(" << this
		<< " sd=" << sd
		<< " pgs=" << peer_global_seq
		<< " cs=" << connect_seq
		<< " l=" << policy.lossy
		<< ").";
}

static int get_proto_version(int my_type, int peer_type, bool connect)
{
  // set reply protocol version
  if (peer_type == my_type) {
    // internal
    switch (my_type) {
    case CEPH_ENTITY_TYPE_OSD: return CEPH_OSD_PROTOCOL;
    case CEPH_ENTITY_TYPE_MDS: return CEPH_MDS_PROTOCOL;
    case CEPH_ENTITY_TYPE_MON: return CEPH_MON_PROTOCOL;
    }
  } else {
    // public
    if (connect) {
      switch (peer_type) {
      case CEPH_ENTITY_TYPE_OSD: return CEPH_OSDC_PROTOCOL;
      case CEPH_ENTITY_TYPE_MDS: return CEPH_MDSC_PROTOCOL;
      case CEPH_ENTITY_TYPE_MON: return CEPH_MONC_PROTOCOL;
      }
    } else {
      switch (my_type) {
      case CEPH_ENTITY_TYPE_OSD: return CEPH_OSDC_PROTOCOL;
      case CEPH_ENTITY_TYPE_MDS: return CEPH_MDSC_PROTOCOL;
      case CEPH_ENTITY_TYPE_MON: return CEPH_MONC_PROTOCOL;
      }
    }
  }
  return 0;
}

void SimpleMessenger::Pipe::queue_received(Message *m, int priority)
{
  assert(pipe_lock.is_locked());
  
  list<Message *>& queue = in_q[priority];
  
  if (halt_delivery)
    goto halt;
  
  if (queue.empty()) {
    // queue pipe AND message under pipe AND dispatch_queue locks.
    pipe_lock.Unlock();
    msgr->dispatch_queue.lock.Lock();
    pipe_lock.Lock();

    if (halt_delivery) {
      msgr->dispatch_queue.lock.Unlock();
      goto halt;
    }

    if (queue.empty()) {
      ldout(msgr->cct,20) << "queue_received queuing pipe" << dendl;
      if (!queue_items.count(priority)) 
	queue_items[priority] = new xlist<Pipe *>::item(this);
      if (msgr->dispatch_queue.queued_pipes.empty())
	msgr->dispatch_queue.cond.Signal();

      map<int, xlist<Pipe*>*>::iterator p = msgr->dispatch_queue.queued_pipes.find(priority);
      xlist<Pipe*> *pipe_list;
      if (p != msgr->dispatch_queue.queued_pipes.end())
	pipe_list = p->second;
      else {
	pipe_list = new xlist<Pipe*>;
	msgr->dispatch_queue.queued_pipes[priority] = pipe_list;
      }
      pipe_list->push_back(queue_items[priority]);
    }

    queue.push_back(m);

    msgr->dispatch_queue.lock.Unlock();
  } else {
    // just queue message under pipe lock.
    queue.push_back(m);
  }
  
  // increment queue length counters
  in_qlen++;
  msgr->dispatch_queue.qlen.inc();
  
  return;
  
 halt:
  // don't want to put local-delivery signals
  // this magic number should be larger than
  // the size of the D_CONNECT et al enum
  if (m>(void *)5) {
    msgr->dispatch_throttle_release(m->get_dispatch_throttle_size());
    m->put();
  }
}



int SimpleMessenger::Pipe::accept()
{
  ldout(msgr->cct,10) << "accept" << dendl;

  // my creater gave me sd via accept()
  assert(state == STATE_ACCEPTING);
  
  // announce myself.
  int rc = tcp_write(msgr->cct, sd, CEPH_BANNER, strlen(CEPH_BANNER));
  if (rc < 0) {
    ldout(msgr->cct,10) << "accept couldn't write banner" << dendl;
    state = STATE_CLOSED;
    return -1;
  }

  // and my addr
  bufferlist addrs;
  ::encode(msgr->ms_addr, addrs);

  // and peer's socket addr (they might not know their ip)
  entity_addr_t socket_addr;
  socklen_t len = sizeof(socket_addr.ss_addr());
  int r = ::getpeername(sd, (sockaddr*)&socket_addr.ss_addr(), &len);
  if (r < 0) {
    char buf[80];
    ldout(msgr->cct,0) << "accept failed to getpeername " << errno << " " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    state = STATE_CLOSED;
    return -1;
  }
  ::encode(socket_addr, addrs);

  rc = tcp_write(msgr->cct, sd, addrs.c_str(), addrs.length());
  if (rc < 0) {
    ldout(msgr->cct,10) << "accept couldn't write my+peer addr" << dendl;
    state = STATE_CLOSED;
    return -1;
  }

  ldout(msgr->cct,1) << "accept sd=" << sd << dendl;
  
  // identify peer
  char banner[strlen(CEPH_BANNER)+1];
  rc = tcp_read(msgr->cct, sd, banner, strlen(CEPH_BANNER), msgr->timeout);
  if (rc < 0) {
    ldout(msgr->cct,10) << "accept couldn't read banner" << dendl;
    state = STATE_CLOSED;
    return -1;
  }
  if (memcmp(banner, CEPH_BANNER, strlen(CEPH_BANNER))) {
    banner[strlen(CEPH_BANNER)] = 0;
    ldout(msgr->cct,1) << "accept peer sent bad banner '" << banner << "' (should be '" << CEPH_BANNER << "')" << dendl;
    state = STATE_CLOSED;
    return -1;
  }
  bufferlist addrbl;
  {
    bufferptr tp(sizeof(peer_addr));
    addrbl.push_back(tp);
  }
  rc = tcp_read(msgr->cct, sd, addrbl.c_str(), addrbl.length(), msgr->timeout);
  if (rc < 0) {
    ldout(msgr->cct,10) << "accept couldn't read peer_addr" << dendl;
    state = STATE_CLOSED;
    return -1;
  }
  {
    bufferlist::iterator ti = addrbl.begin();
    ::decode(peer_addr, ti);
  }

  ldout(msgr->cct,10) << "accept peer addr is " << peer_addr << dendl;
  if (peer_addr.is_blank_ip()) {
    // peer apparently doesn't know what ip they have; figure it out for them.
    int port = peer_addr.get_port();
    peer_addr.addr = socket_addr.addr;
    peer_addr.set_port(port);
    ldout(msgr->cct,0) << "accept peer addr is really " << peer_addr
	    << " (socket is " << socket_addr << ")" << dendl;
  }
  set_peer_addr(peer_addr);  // so that connection_state gets set up
  
  ceph_msg_connect connect;
  ceph_msg_connect_reply reply;
  Pipe *existing = 0;
  bufferptr bp;
  bufferlist authorizer, authorizer_reply;
  bool authorizer_valid;
  uint64_t feat_missing;

  // this should roughly mirror pseudocode at
  //  http://ceph.newdream.net/wiki/Messaging_protocol
  int reply_tag = 0;
  uint64_t existing_seq = -1;
  while (1) {
    rc = tcp_read(msgr->cct, sd, (char*)&connect, sizeof(connect), msgr->timeout);
    if (rc < 0) {
      ldout(msgr->cct,10) << "accept couldn't read connect" << dendl;
      goto fail_unlocked;
    }


    authorizer.clear();
    if (connect.authorizer_len) {
      bp = buffer::create(connect.authorizer_len);
      if (tcp_read(msgr->cct, sd, bp.c_str(), connect.authorizer_len, msgr->timeout) < 0) {
        ldout(msgr->cct,10) << "accept couldn't read connect authorizer" << dendl;
        goto fail_unlocked;
      }
      authorizer.push_back(bp);
      authorizer_reply.clear();
    }

    ldout(msgr->cct,20) << "accept got peer connect_seq " << connect.connect_seq
	     << " global_seq " << connect.global_seq
	     << dendl;
    
    msgr->lock.Lock();

    // note peer's type, flags
    set_peer_type(connect.host_type);
    policy = msgr->get_policy(connect.host_type);
    ldout(msgr->cct,10) << "accept of host_type " << connect.host_type
	     << ", policy.lossy=" << policy.lossy
	     << dendl;

    memset(&reply, 0, sizeof(reply));
    reply.protocol_version = get_proto_version(msgr->my_type, peer_type, false);

    // mismatch?
    ldout(msgr->cct,10) << "accept my proto " << reply.protocol_version
	     << ", their proto " << connect.protocol_version << dendl;
    if (connect.protocol_version != reply.protocol_version) {
      reply.tag = CEPH_MSGR_TAG_BADPROTOVER;
      msgr->lock.Unlock();
      goto reply;
    }

    feat_missing = policy.features_required & ~(uint64_t)connect.features;
    if (feat_missing) {
      ldout(msgr->cct,1) << "peer missing required features " << std::hex << feat_missing << std::dec << dendl;
      reply.tag = CEPH_MSGR_TAG_FEATURES;
      msgr->lock.Unlock();
      goto reply;
    }
    
    msgr->lock.Unlock();
    if (msgr->verify_authorizer(connection_state, peer_type,
				connect.authorizer_protocol, authorizer, authorizer_reply, authorizer_valid) &&
	!authorizer_valid) {
      ldout(msgr->cct,0) << "accept bad authorizer" << dendl;
      reply.tag = CEPH_MSGR_TAG_BADAUTHORIZER;
      goto reply;
    }
    msgr->lock.Lock();
    
    // existing?
    if (msgr->rank_pipe.count(peer_addr)) {
      existing = msgr->rank_pipe[peer_addr];
      existing->pipe_lock.Lock();

      if (connect.global_seq < existing->peer_global_seq) {
	ldout(msgr->cct,10) << "accept existing " << existing << ".gseq " << existing->peer_global_seq
		 << " > " << connect.global_seq << ", RETRY_GLOBAL" << dendl;
	reply.tag = CEPH_MSGR_TAG_RETRY_GLOBAL;
	reply.global_seq = existing->peer_global_seq;  // so we can send it below..
	existing->pipe_lock.Unlock();
	msgr->lock.Unlock();
	goto reply;
      } else {
	ldout(msgr->cct,10) << "accept existing " << existing << ".gseq " << existing->peer_global_seq
		 << " <= " << connect.global_seq << ", looks ok" << dendl;
      }
      
      if (existing->policy.lossy) {
	ldout(msgr->cct,0) << "accept replacing existing (lossy) channel (new one lossy="
	        << policy.lossy << ")" << dendl;
	existing->was_session_reset();
	goto replace;
      }
      /*if (lossy_rx) {
	if (existing->state == STATE_STANDBY) {
	  ldout(msgr->cct,0) << "accept incoming lossy connection, kicking outgoing lossless "
		    << existing << dendl;
	  existing->state = STATE_CONNECTING;
	  existing->cond.Signal();
	} else {
	  ldout(msgr->cct,0) << "accept incoming lossy connection, our lossless " << existing
		    << " has state " << existing->state << ", doing nothing" << dendl;
	}
	existing->lock.Unlock();
	goto fail;
	}*/

      ldout(msgr->cct,0) << "accept connect_seq " << connect.connect_seq
		<< " vs existing " << existing->connect_seq
		<< " state " << existing->state << dendl;

      if (connect.connect_seq < existing->connect_seq) {
	if (connect.connect_seq == 0) {
	  ldout(msgr->cct,0) << "accept peer reset, then tried to connect to us, replacing" << dendl;
	  existing->was_session_reset(); // this resets out_queue, msg_ and connect_seq #'s
	  goto replace;
	} else {
	  // old attempt, or we sent READY but they didn't get it.
	  ldout(msgr->cct,10) << "accept existing " << existing << ".cseq " << existing->connect_seq
		   << " > " << connect.connect_seq << ", RETRY_SESSION" << dendl;
	  reply.tag = CEPH_MSGR_TAG_RETRY_SESSION;
	  reply.connect_seq = existing->connect_seq;  // so we can send it below..
	  existing->pipe_lock.Unlock();
	  msgr->lock.Unlock();
	  goto reply;
	}
      }

      if (connect.connect_seq == existing->connect_seq) {
	// connection race?
	if (peer_addr < msgr->ms_addr ||
	    existing->policy.server) {
	  // incoming wins
	  ldout(msgr->cct,10) << "accept connection race, existing " << existing << ".cseq " << existing->connect_seq
		   << " == " << connect.connect_seq << ", or we are server, replacing my attempt" << dendl;
	  assert(existing->state == STATE_CONNECTING ||
		 existing->state == STATE_STANDBY ||
		 existing->state == STATE_WAIT);
	  goto replace;
	} else {
	  // our existing outgoing wins
	  ldout(msgr->cct,10) << "accept connection race, existing " << existing << ".cseq " << existing->connect_seq
		   << " == " << connect.connect_seq << ", sending WAIT" << dendl;
	  assert(peer_addr > msgr->ms_addr);
	  assert(existing->state == STATE_CONNECTING ||
		 existing->state == STATE_OPEN); // this will win
	  reply.tag = CEPH_MSGR_TAG_WAIT;
	  existing->pipe_lock.Unlock();
	  msgr->lock.Unlock();
	  goto reply;
	}
      }

      assert(connect.connect_seq > existing->connect_seq);
      assert(connect.global_seq >= existing->peer_global_seq);
      if (existing->connect_seq == 0) {
	ldout(msgr->cct,0) << "accept we reset (peer sent cseq " << connect.connect_seq 
		 << ", " << existing << ".cseq = " << existing->connect_seq
		 << "), sending RESETSESSION" << dendl;
	reply.tag = CEPH_MSGR_TAG_RESETSESSION;
	msgr->lock.Unlock();
	existing->pipe_lock.Unlock();
	goto reply;
      }

      // reconnect
      ldout(msgr->cct,10) << "accept peer sent cseq " << connect.connect_seq
	       << " > " << existing->connect_seq << dendl;
      goto replace;
    } // existing
    else if (connect.connect_seq > 0) {
      // we reset, and they are opening a new session
      ldout(msgr->cct,0) << "accept we reset (peer sent cseq " << connect.connect_seq << "), sending RESETSESSION" << dendl;
      msgr->lock.Unlock();
      reply.tag = CEPH_MSGR_TAG_RESETSESSION;
      goto reply;
    } else {
      // new session
      ldout(msgr->cct,10) << "accept new session" << dendl;
      existing = NULL;
      goto open;
    }
    assert(0);    

  reply:
    reply.features = ((uint64_t)connect.features & policy.features_supported) | policy.features_required;
    reply.authorizer_len = authorizer_reply.length();
    rc = tcp_write(msgr->cct, sd, (char*)&reply, sizeof(reply));
    if (rc < 0)
      goto fail_unlocked;
    if (reply.authorizer_len) {
      rc = tcp_write(msgr->cct, sd, authorizer_reply.c_str(), authorizer_reply.length());
      if (rc < 0)
	goto fail_unlocked;
    }
  }
  
 replace:
  if (connect.features & CEPH_FEATURE_RECONNECT_SEQ) {
    reply_tag = CEPH_MSGR_TAG_SEQ;
    existing_seq = existing->in_seq;
  }
  ldout(msgr->cct,10) << "accept replacing " << existing << dendl;
  existing->stop();
  existing->unregister_pipe();
    
  if (!existing->policy.lossy) { /* if we're lossy, we can lose messages and
                                    should let the daemon handle it itself.
    Otherwise, take over other Connection so we don't lose older messages */
    existing->connection_state->reset_pipe(this);

    // do not clear existing->connection_state, since read_message and write_message both
    // dereference it without pipe_lock.

    // steal queue and out_seq
    existing->requeue_sent();
    out_seq = existing->out_seq;
    in_seq = existing->in_seq;
    in_seq_acked = in_seq;
    ldout(msgr->cct,10) << "accept re-queuing on out_seq " << out_seq << " in_seq " << in_seq << dendl;
    for (map<int, list<Message*> >::iterator p = existing->out_q.begin();
         p != existing->out_q.end();
         p++)
      out_q[p->first].splice(out_q[p->first].begin(), p->second);
  }
  existing->pipe_lock.Unlock();

 open:
  // open
  connect_seq = connect.connect_seq + 1;
  peer_global_seq = connect.global_seq;
  state = STATE_OPEN;
  ldout(msgr->cct,10) << "accept success, connect_seq = " << connect_seq << ", sending READY" << dendl;

  // send READY reply
  reply.tag = (reply_tag ? reply_tag : CEPH_MSGR_TAG_READY);
  reply.features = policy.features_supported;
  reply.global_seq = msgr->get_global_seq();
  reply.connect_seq = connect_seq;
  reply.flags = 0;
  reply.authorizer_len = authorizer_reply.length();
  if (policy.lossy)
    reply.flags = reply.flags | CEPH_MSG_CONNECT_LOSSY;

  connection_state->set_features((int)reply.features & (int)connect.features);
  ldout(msgr->cct,10) << "accept features " << connection_state->get_features() << dendl;

  // ok!
  register_pipe();
  msgr->lock.Unlock();

  rc = tcp_write(msgr->cct, sd, (char*)&reply, sizeof(reply));
  if (rc < 0) {
    goto fail_unlocked;
  }

  if (reply.authorizer_len) {
    rc = tcp_write(msgr->cct, sd, authorizer_reply.c_str(), authorizer_reply.length());
    if (rc < 0) {
      goto fail_unlocked;
    }
  }

  if (reply_tag == CEPH_MSGR_TAG_SEQ) {
    uint64_t newly_acked_seq = 0;
    if(tcp_write(msgr->cct, sd, (char*)&existing_seq, sizeof(existing_seq)) < 0) {
      ldout(msgr->cct,2) << "accept write error on in_seq" << dendl;
      goto fail_unlocked;
    }
    if(tcp_read(msgr->cct, sd, (char*)&newly_acked_seq, sizeof(newly_acked_seq)) < 0) {
      ldout(msgr->cct,2) << "accept read error on newly_acked_seq" << dendl;
      goto fail_unlocked;
    }
    requeue_sent(newly_acked_seq);
  }

  pipe_lock.Lock();
  if (state != STATE_CLOSED) {
    ldout(msgr->cct,10) << "accept starting writer, " << "state=" << state << dendl;
    start_writer();
  }
  ldout(msgr->cct,20) << "accept done" << dendl;
  pipe_lock.Unlock();
  return 0;   // success.

 fail_unlocked:
  pipe_lock.Lock();
  bool queued = is_queued();
  if (queued)
    state = STATE_CONNECTING;
  else
    state = STATE_CLOSED;
  fault();
  if (queued)
    start_writer();
  pipe_lock.Unlock();
  return -1;
}

int SimpleMessenger::Pipe::connect()
{
  bool got_bad_auth = false;

  ldout(msgr->cct,10) << "connect " << connect_seq << dendl;
  assert(pipe_lock.is_locked());

  __u32 cseq = connect_seq;
  __u32 gseq = msgr->get_global_seq();

  // stop reader thrad
  join_reader();

  pipe_lock.Unlock();
  
  char tag = -1;
  int rc;
  struct msghdr msg;
  struct iovec msgvec[2];
  int msglen;
  char banner[strlen(CEPH_BANNER)];
  entity_addr_t paddr;
  entity_addr_t peer_addr_for_me, socket_addr;
  AuthAuthorizer *authorizer = NULL;
  bufferlist addrbl, myaddrbl;
  const md_config_t *conf = msgr->cct->_conf;

  // close old socket.  this is safe because we stopped the reader thread above.
  if (sd >= 0)
    ::close(sd);

  // create socket?
  sd = ::socket(peer_addr.get_family(), SOCK_STREAM, 0);
  if (sd < 0) {
    char buf[80];
    lderr(msgr->cct) << "connect couldn't created socket " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    assert(0);
    goto fail;
  }

  char buf[80];

  // connect!
  ldout(msgr->cct,10) << "connecting to " << peer_addr << dendl;
  rc = ::connect(sd, (sockaddr*)&peer_addr.addr, peer_addr.addr_size());
  if (rc < 0) {
    ldout(msgr->cct,2) << "connect error " << peer_addr
	     << ", " << errno << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }

  // disable Nagle algorithm?
  if (conf->ms_tcp_nodelay) {
    int flag = 1;
    int r = ::setsockopt(sd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
    if (r < 0) 
      ldout(msgr->cct,0) << "connect couldn't set TCP_NODELAY: " << strerror_r(errno, buf, sizeof(buf)) << dendl;
  }

  // verify banner
  // FIXME: this should be non-blocking, or in some other way verify the banner as we get it.
  rc = tcp_read(msgr->cct, sd, (char*)&banner, strlen(CEPH_BANNER), msgr->timeout);
  if (rc < 0) {
    ldout(msgr->cct,2) << "connect couldn't read banner, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }
  if (memcmp(banner, CEPH_BANNER, strlen(CEPH_BANNER))) {
    ldout(msgr->cct,0) << "connect protocol error (bad banner) on peer " << paddr << dendl;
    goto fail;
  }

  memset(&msg, 0, sizeof(msg));
  msgvec[0].iov_base = banner;
  msgvec[0].iov_len = strlen(CEPH_BANNER);
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 1;
  msglen = msgvec[0].iov_len;
  if (do_sendmsg(sd, &msg, msglen)) {
    ldout(msgr->cct,2) << "connect couldn't write my banner, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }

  // identify peer
  {
    bufferptr p(sizeof(paddr) * 2);
    addrbl.push_back(p);
  }
  rc = tcp_read(msgr->cct, sd, addrbl.c_str(), addrbl.length(), msgr->timeout);
  if (rc < 0) {
    ldout(msgr->cct,2) << "connect couldn't read peer addrs, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }
  {
    bufferlist::iterator p = addrbl.begin();
    ::decode(paddr, p);
    ::decode(peer_addr_for_me, p);
  }

  ldout(msgr->cct,20) << "connect read peer addr " << paddr << " on socket " << sd << dendl;
  if (peer_addr != paddr) {
    if (paddr.is_blank_ip() &&
	peer_addr.get_port() == paddr.get_port() &&
	peer_addr.get_nonce() == paddr.get_nonce()) {
      ldout(msgr->cct,0) << "connect claims to be " 
	      << paddr << " not " << peer_addr << " - presumably this is the same node!" << dendl;
    } else {
      ldout(msgr->cct,0) << "connect claims to be " 
	      << paddr << " not " << peer_addr << " - wrong node!" << dendl;
      goto fail;
    }
  }

  ldout(msgr->cct,20) << "connect peer addr for me is " << peer_addr_for_me << dendl;

  if (msgr->need_addr)
    msgr->learned_addr(peer_addr_for_me);

  ::encode(msgr->ms_addr, myaddrbl);

  memset(&msg, 0, sizeof(msg));
  msgvec[0].iov_base = myaddrbl.c_str();
  msgvec[0].iov_len = myaddrbl.length();
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 1;
  msglen = msgvec[0].iov_len;
  if (do_sendmsg(sd, &msg, msglen)) {
    ldout(msgr->cct,2) << "connect couldn't write my addr, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }
  ldout(msgr->cct,10) << "connect sent my addr " << msgr->ms_addr << dendl;


  while (1) {
    delete authorizer;
    authorizer = msgr->get_authorizer(peer_type, false);
    bufferlist authorizer_reply;

    ceph_msg_connect connect;
    connect.features = policy.features_supported;
    connect.host_type = msgr->my_type;
    connect.global_seq = gseq;
    connect.connect_seq = cseq;
    connect.protocol_version = get_proto_version(msgr->my_type, peer_type, true);
    connect.authorizer_protocol = authorizer ? authorizer->protocol : 0;
    connect.authorizer_len = authorizer ? authorizer->bl.length() : 0;
    if (authorizer) 
      ldout(msgr->cct,10) << "connect.authorizer_len=" << connect.authorizer_len
	       << " protocol=" << connect.authorizer_protocol << dendl;
    connect.flags = 0;
    if (policy.lossy)
      connect.flags |= CEPH_MSG_CONNECT_LOSSY;  // this is fyi, actually, server decides!
    memset(&msg, 0, sizeof(msg));
    msgvec[0].iov_base = (char*)&connect;
    msgvec[0].iov_len = sizeof(connect);
    msg.msg_iov = msgvec;
    msg.msg_iovlen = 1;
    msglen = msgvec[0].iov_len;
    if (authorizer) {
      msgvec[1].iov_base = authorizer->bl.c_str();
      msgvec[1].iov_len = authorizer->bl.length();
      msg.msg_iovlen++;
      msglen += msgvec[1].iov_len;
    }

    ldout(msgr->cct,10) << "connect sending gseq=" << gseq << " cseq=" << cseq
	     << " proto=" << connect.protocol_version << dendl;
    if (do_sendmsg(sd, &msg, msglen)) {
      ldout(msgr->cct,2) << "connect couldn't write gseq, cseq, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      goto fail;
    }

    ldout(msgr->cct,20) << "connect wrote (self +) cseq, waiting for reply" << dendl;
    ceph_msg_connect_reply reply;
    if (tcp_read(msgr->cct, sd, (char*)&reply, sizeof(reply), msgr->timeout) < 0) {
      ldout(msgr->cct,2) << "connect read reply " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      goto fail;
    }
    ldout(msgr->cct,20) << "connect got reply tag " << (int)reply.tag
	     << " connect_seq " << reply.connect_seq
	     << " global_seq " << reply.global_seq
	     << " proto " << reply.protocol_version
	     << " flags " << (int)reply.flags
	     << dendl;

    authorizer_reply.clear();

    if (reply.authorizer_len) {
      ldout(msgr->cct,10) << "reply.authorizer_len=" << reply.authorizer_len << dendl;
      bufferptr bp = buffer::create(reply.authorizer_len);
      if (tcp_read(msgr->cct, sd, bp.c_str(), reply.authorizer_len, msgr->timeout) < 0) {
        ldout(msgr->cct,10) << "connect couldn't read connect authorizer_reply" << dendl;
	goto fail;
      }
      authorizer_reply.push_back(bp);
    }

    if (authorizer) {
      bufferlist::iterator iter = authorizer_reply.begin();
      if (!authorizer->verify_reply(iter)) {
        ldout(msgr->cct,0) << "failed verifying authorize reply" << dendl;
	goto fail;
      }
    }

    pipe_lock.Lock();
    if (state != STATE_CONNECTING) {
      ldout(msgr->cct,0) << "connect got RESETSESSION but no longer connecting" << dendl;
      goto stop_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_FEATURES) {
      ldout(msgr->cct,0) << "connect protocol feature mismatch, my " << std::hex
	      << connect.features << " < peer " << reply.features
	      << " missing " << (reply.features & ~policy.features_supported)
	      << std::dec << dendl;
      goto fail_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_BADPROTOVER) {
      ldout(msgr->cct,0) << "connect protocol version mismatch, my " << connect.protocol_version
	      << " != " << reply.protocol_version << dendl;
      goto fail_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_BADAUTHORIZER) {
      ldout(msgr->cct,0) << "connect got BADAUTHORIZER" << dendl;
      if (got_bad_auth)
        goto stop_locked;
      got_bad_auth = true;
      pipe_lock.Unlock();
      authorizer = msgr->get_authorizer(peer_type, true);  // try harder
      continue;
    }
    if (reply.tag == CEPH_MSGR_TAG_RESETSESSION) {
      ldout(msgr->cct,0) << "connect got RESETSESSION" << dendl;
      was_session_reset();
      halt_delivery = false;
      cseq = 0;
      pipe_lock.Unlock();
      continue;
    }
    if (reply.tag == CEPH_MSGR_TAG_RETRY_GLOBAL) {
      gseq = msgr->get_global_seq(reply.global_seq);
      ldout(msgr->cct,10) << "connect got RETRY_GLOBAL " << reply.global_seq
	       << " chose new " << gseq << dendl;
      pipe_lock.Unlock();
      continue;
    }
    if (reply.tag == CEPH_MSGR_TAG_RETRY_SESSION) {
      assert(reply.connect_seq > connect_seq);
      ldout(msgr->cct,10) << "connect got RETRY_SESSION " << connect_seq
	       << " -> " << reply.connect_seq << dendl;
      cseq = connect_seq = reply.connect_seq;
      pipe_lock.Unlock();
      continue;
    }

    if (reply.tag == CEPH_MSGR_TAG_WAIT) {
      ldout(msgr->cct,3) << "connect got WAIT (connection race)" << dendl;
      state = STATE_WAIT;
      goto stop_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_READY ||
        reply.tag == CEPH_MSGR_TAG_SEQ) {
      uint64_t feat_missing = policy.features_required & ~(uint64_t)reply.features;
      if (feat_missing) {
	ldout(msgr->cct,1) << "missing required features " << std::hex << feat_missing << std::dec << dendl;
	goto fail_locked;
      }

      if (reply.tag == CEPH_MSGR_TAG_SEQ) {
        ldout(msgr->cct,10) << "got CEPH_MSGR_TAG_SEQ, reading acked_seq and writing in_seq" << dendl;
        uint64_t newly_acked_seq = 0;
        if (tcp_read(msgr->cct, sd, (char*)&newly_acked_seq, sizeof(newly_acked_seq)) < 0) {
          ldout(msgr->cct,2) << "connect read error on newly_acked_seq" << dendl;
          goto fail_locked;
        }
        handle_ack(newly_acked_seq);
        if (tcp_write(msgr->cct, sd, (char*)&in_seq, sizeof(in_seq)) < 0) {
          ldout(msgr->cct,2) << "connect write error on in_seq" << dendl;
          goto fail_locked;
        }
      }

      // hooray!
      peer_global_seq = reply.global_seq;
      if (!disposable) {
	policy.lossy = reply.flags & CEPH_MSG_CONNECT_LOSSY;
      }
      state = STATE_OPEN;
      connect_seq = cseq + 1;
      assert(connect_seq == reply.connect_seq);
      backoff = utime_t();
      connection_state->set_features((unsigned)reply.features & (unsigned)connect.features);
      ldout(msgr->cct,10) << "connect success " << connect_seq << ", lossy = " << policy.lossy
	       << ", features " << connection_state->get_features() << dendl;
      
      if (!msgr->destination_stopped) {
	Connection * cstate = connection_state->get();
	pipe_lock.Unlock();
	msgr->dispatch_queue.queue_connect(cstate);
	pipe_lock.Lock();
      }
      
      if (!reader_running) {
	ldout(msgr->cct,20) << "connect starting reader" << dendl;
	start_reader();
      }
      delete authorizer;
      return 0;
    }
    
    // protocol error
    ldout(msgr->cct,0) << "connect got bad tag " << (int)tag << dendl;
    goto fail_locked;
  }

 fail:
  pipe_lock.Lock();
 fail_locked:
  if (state == STATE_CONNECTING)
    fault(true);
  else
    ldout(msgr->cct,3) << "connect fault, but state != connecting, stopping" << dendl;

 stop_locked:
  delete authorizer;
  return -1;
}

void SimpleMessenger::Pipe::register_pipe()
{
  ldout(msgr->cct,10) << "register_pipe" << dendl;
  assert(msgr->lock.is_locked());
  assert(msgr->rank_pipe.count(peer_addr) == 0);
  msgr->rank_pipe[peer_addr] = this;
}

void SimpleMessenger::Pipe::unregister_pipe()
{
  assert(msgr->lock.is_locked());
  if (msgr->rank_pipe.count(peer_addr) &&
      msgr->rank_pipe[peer_addr] == this) {
    ldout(msgr->cct,10) << "unregister_pipe" << dendl;
    msgr->rank_pipe.erase(peer_addr);
  } else {
    ldout(msgr->cct,10) << "unregister_pipe - not registered" << dendl;
  }
}


void SimpleMessenger::Pipe::requeue_sent(uint64_t max_acked)
{
  if (sent.empty())
    return;

  list<Message*>& rq = out_q[CEPH_MSG_PRIO_HIGHEST];
  while (!sent.empty()) {
    Message *m = sent.back();
    if (m->get_seq() > max_acked) {
      sent.pop_back();
      ldout(msgr->cct,10) << "requeue_sent " << *m << " for resend seq " << out_seq
          << " (" << m->get_seq() << ")" << dendl;
      rq.push_front(m);
      out_seq--;
    } else
      sent.clear();
  }
}

/*
 * Tears down the Pipe's message queues, and removes them from the DispatchQueue
 * Must hold pipe_lock prior to calling.
 */
void SimpleMessenger::Pipe::discard_queue()
{
  ldout(msgr->cct,10) << "discard_queue" << dendl;

  halt_delivery = true;

  // dequeue pipe
  DispatchQueue& q = msgr->dispatch_queue;
  pipe_lock.Unlock();
  q.lock.Lock();
  pipe_lock.Lock();
  for (map<int, xlist<Pipe *>::item* >::iterator i = queue_items.begin();
       i != queue_items.end();
       ++i) {
    xlist<Pipe *>* list_on;
    if ((list_on = i->second->get_list())) { //if in round-robin
      i->second->remove_myself(); //take off
      if (list_on->empty()) { //if round-robin queue is empty
	delete list_on;
	q.queued_pipes.erase(i->first); //remove from map
      }
    }
  }

  // clear queue_items
  while (!queue_items.empty()) {
    delete queue_items.begin()->second;
    queue_items.erase(queue_items.begin());
  }

  q.lock.Unlock();

  ldout(msgr->cct,20) << " dequeued pipe " << dendl;

  // adjust qlen
  q.qlen.sub(in_qlen);

  for (list<Message*>::iterator p = sent.begin(); p != sent.end(); p++) {
    ldout(msgr->cct,20) << "  discard " << *p << dendl;
    (*p)->put();
  }
  sent.clear();
  for (map<int,list<Message*> >::iterator p = out_q.begin(); p != out_q.end(); p++)
    for (list<Message*>::iterator r = p->second.begin(); r != p->second.end(); r++) {
      ldout(msgr->cct,20) << "  discard " << *r << dendl;
      (*r)->put();
    }
  out_q.clear();
  for (map<int,list<Message*> >::iterator p = in_q.begin(); p != in_q.end(); p++)
    for (list<Message*>::iterator r = p->second.begin(); r != p->second.end(); r++) {
      msgr->dispatch_throttle_release((*r)->get_dispatch_throttle_size());
      ldout(msgr->cct,20) << "  discard " << *r << dendl;
      (*r)->put();
    }
  in_q.clear();
  in_qlen = 0;
}


void SimpleMessenger::Pipe::fault(bool onconnect, bool onread)
{
  const md_config_t *conf = msgr->cct->_conf;
  assert(pipe_lock.is_locked());
  cond.Signal();

  if (onread && state == STATE_CONNECTING) {
    ldout(msgr->cct,10) << "fault already connecting, reader shutting down" << dendl;
    return;
  }
  
  char buf[80];
  if (!onconnect) ldout(msgr->cct,2) << "fault " << errno << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;

  if (state == STATE_CLOSED ||
      state == STATE_CLOSING) {
    ldout(msgr->cct,10) << "fault already closed|closing" << dendl;
    return;
  }

  shutdown_socket();

  // lossy channel?
  if (policy.lossy) {
    ldout(msgr->cct,10) << "fault on lossy channel, failing" << dendl;
    fail();
    return;
  }

  // requeue sent items
  requeue_sent();

  if (!is_queued()) {
    if (state == STATE_CLOSING || onconnect) {
      ldout(msgr->cct,10) << "fault on connect, or already closing, and q empty: setting closed." << dendl;
      state = STATE_CLOSED;
    } else {
      ldout(msgr->cct,0) << "fault with nothing to send, going to standby" << dendl;
      state = STATE_STANDBY;
    }
    return;
  } 


  if (state != STATE_CONNECTING) {
    if (!onconnect)
      ldout(msgr->cct,0) << "fault initiating reconnect" << dendl;
    connect_seq++;
    state = STATE_CONNECTING;
    backoff = utime_t();
  } else if (backoff == utime_t()) {
    if (!onconnect)
      ldout(msgr->cct,0) << "fault first fault" << dendl;
    backoff.set_from_double(conf->ms_initial_backoff);
  } else {
    ldout(msgr->cct,10) << "fault waiting " << backoff << dendl;
    cond.WaitInterval(msgr->cct, pipe_lock, backoff);
    backoff += backoff;
    if (backoff > conf->ms_max_backoff)
      backoff.set_from_double(conf->ms_max_backoff);
    ldout(msgr->cct,10) << "fault done waiting or woke up" << dendl;
  }
}

void SimpleMessenger::Pipe::fail()
{
  ldout(msgr->cct,10) << "fail" << dendl;
  assert(pipe_lock.is_locked());

  stop();

  discard_queue();
  
  if (!msgr->destination_stopped) {
    Connection * cstate = connection_state->get();
    pipe_lock.Unlock();
    msgr->dispatch_queue.queue_reset(cstate);
    pipe_lock.Lock();
  }
}

void SimpleMessenger::Pipe::was_session_reset()
{
  assert(pipe_lock.is_locked());

  ldout(msgr->cct,10) << "was_session_reset" << dendl;
  discard_queue();

  if (!msgr->destination_stopped) {
    Connection * cstate = connection_state->get();
    pipe_lock.Unlock();
    msgr->dispatch_queue.queue_remote_reset(cstate);
    pipe_lock.Lock();
  }

  out_seq = 0;
  in_seq = 0;
  connect_seq = 0;
}

void SimpleMessenger::Pipe::stop()
{
  ldout(msgr->cct,10) << "stop" << dendl;
  assert(pipe_lock.is_locked());
  state = STATE_CLOSED;
  cond.Signal();
  shutdown_socket();
}


/* read msgs from socket.
 * also, server.
 */
void SimpleMessenger::Pipe::reader()
{
  if (state == STATE_ACCEPTING) 
    accept();

  pipe_lock.Lock();

  // loop.
  while (state != STATE_CLOSED &&
	 state != STATE_CONNECTING) {
    assert(pipe_lock.is_locked());

    // sleep if (re)connecting
    if (state == STATE_STANDBY) {
      ldout(msgr->cct,20) << "reader sleeping during reconnect|standby" << dendl;
      cond.Wait(pipe_lock);
      continue;
    }

    pipe_lock.Unlock();

    char buf[80];
    char tag = -1;
    ldout(msgr->cct,20) << "reader reading tag..." << dendl;
    int rc = tcp_read(msgr->cct, sd, (char*)&tag, 1, msgr->timeout);
    if (rc < 0) {
      pipe_lock.Lock();
      ldout(msgr->cct,2) << "reader couldn't read tag, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      fault(false, true);
      continue;
    }

    if (tag == CEPH_MSGR_TAG_KEEPALIVE) {
      ldout(msgr->cct,20) << "reader got KEEPALIVE" << dendl;
      pipe_lock.Lock();
      continue;
    }

    // open ...
    if (tag == CEPH_MSGR_TAG_ACK) {
      ldout(msgr->cct,20) << "reader got ACK" << dendl;
      ceph_le64 seq;
      int rc = tcp_read(msgr->cct,  sd, (char*)&seq, sizeof(seq), msgr->timeout);
      pipe_lock.Lock();
      if (rc < 0) {
	ldout(msgr->cct,2) << "reader couldn't read ack seq, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	fault(false, true);
      } else if (state != STATE_CLOSED) {
        handle_ack(seq);
      }
      continue;
    }

    else if (tag == CEPH_MSGR_TAG_MSG) {
      ldout(msgr->cct,20) << "reader got MSG" << dendl;
      Message *m = 0;
      int r = read_message(&m);

      pipe_lock.Lock();
      
      if (!m) {
	if (r < 0)
	  fault(false, true);
	continue;
      }

      if (state == STATE_CLOSED ||
	  state == STATE_CONNECTING) {
	msgr->dispatch_throttle_release(m->get_dispatch_throttle_size());
	m->put();
	continue;
      }

      // check received seq#.  if it is old, drop the message.  
      // note that incoming messages may skip ahead.  this is convenient for the client
      // side queueing because messages can't be renumbered, but the (kernel) client will
      // occasionally pull a message out of the sent queue to send elsewhere.  in that case
      // it doesn't matter if we "got" it or not.
      if (m->get_seq() <= in_seq) {
	ldout(msgr->cct,0) << "reader got old message "
		<< m->get_seq() << " <= " << in_seq << " " << m << " " << *m
		<< ", discarding" << dendl;
	msgr->dispatch_throttle_release(m->get_dispatch_throttle_size());
	m->put();
	continue;
      }

      m->set_connection(connection_state->get());

      // note last received message.
      in_seq = m->get_seq();

      cond.Signal();  // wake up writer, to ack this
      
      ldout(msgr->cct,10) << "reader got message "
	       << m->get_seq() << " " << m << " " << *m
	       << dendl;
      queue_received(m);
    } 
    
    else if (tag == CEPH_MSGR_TAG_CLOSE) {
      ldout(msgr->cct,20) << "reader got CLOSE" << dendl;
      pipe_lock.Lock();
      if (state == STATE_CLOSING)
	state = STATE_CLOSED;
      else
	state = STATE_CLOSING;
      cond.Signal();
      break;
    }
    else {
      ldout(msgr->cct,0) << "reader bad tag " << (int)tag << dendl;
      pipe_lock.Lock();
      fault(false, true);
    }
  }

 
  // reap?
  reader_running = false;
  unlock_maybe_reap();
  ldout(msgr->cct,10) << "reader done" << dendl;
}

/* write msgs to socket.
 * also, client.
 */
void SimpleMessenger::Pipe::writer()
{
  char buf[80];

  pipe_lock.Lock();
  while (state != STATE_CLOSED) {// && state != STATE_WAIT) {
    ldout(msgr->cct,10) << "writer: state = " << state << " policy.server=" << policy.server << dendl;

    // standby?
    if (is_queued() && state == STATE_STANDBY && !policy.server) {
      connect_seq++;
      state = STATE_CONNECTING;
    }

    // connect?
    if (state == STATE_CONNECTING) {
      if (policy.server) {
	state = STATE_STANDBY;
      } else {
	connect();
	continue;
      }
    }
    
    if (state == STATE_CLOSING) {
      // write close tag
      ldout(msgr->cct,20) << "writer writing CLOSE tag" << dendl;
      char tag = CEPH_MSGR_TAG_CLOSE;
      state = STATE_CLOSED;
      pipe_lock.Unlock();
      if (sd) {
	int r = ::write(sd, &tag, 1);
	// we can ignore r, actually; we don't care if this succeeds.
	r++; r = 0; // placate gcc
      }
      pipe_lock.Lock();
      continue;
    }

    if (state != STATE_CONNECTING && state != STATE_WAIT && state != STATE_STANDBY &&
	(is_queued() || in_seq > in_seq_acked)) {

      // keepalive?
      if (keepalive) {
	pipe_lock.Unlock();
	int rc = write_keepalive();
	pipe_lock.Lock();
	if (rc < 0) {
	  ldout(msgr->cct,2) << "writer couldn't write keepalive, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	  fault();
 	  continue;
	}
	keepalive = false;
      }

      // send ack?
      if (in_seq > in_seq_acked) {
	uint64_t send_seq = in_seq;
	pipe_lock.Unlock();
	int rc = write_ack(send_seq);
	pipe_lock.Lock();
	if (rc < 0) {
	  ldout(msgr->cct,2) << "writer couldn't write ack, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	  fault();
 	  continue;
	}
	in_seq_acked = send_seq;
      }

      // grab outgoing message
      Message *m = _get_next_outgoing();
      if (m) {
	m->set_seq(++out_seq);
	if (!policy.lossy || close_on_empty) {
	  // put on sent list
	  sent.push_back(m); 
	  m->get();
	}
	pipe_lock.Unlock();

        ldout(msgr->cct,20) << "writer encoding " << m->get_seq() << " " << m << " " << *m << dendl;

	// associate message with Connection (for benefit of encode_payload)
	m->set_connection(connection_state->get());

	// encode and copy out of *m
	m->encode(msgr->cct);

        ldout(msgr->cct,20) << "writer sending " << m->get_seq() << " " << m << dendl;
	int rc = write_message(m);

	pipe_lock.Lock();
	if (rc < 0) {
          ldout(msgr->cct,1) << "writer error sending " << m << ", "
		  << errno << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	  fault();
        }
	m->put();
      }
      continue;
    }
    
    if (sent.empty() && close_on_empty) {
      // this is slightly hacky
      ldout(msgr->cct,10) << "writer out and sent queues empty, closing" << dendl;
      policy.lossy = true;
      fault();
      continue;
    }

    // wait
    ldout(msgr->cct,20) << "writer sleeping" << dendl;
    cond.Wait(pipe_lock);
  }
  
  ldout(msgr->cct,20) << "writer finishing" << dendl;

  // reap?
  writer_running = false;
  unlock_maybe_reap();
  ldout(msgr->cct,10) << "writer done" << dendl;
}

void SimpleMessenger::Pipe::unlock_maybe_reap()
{
  if (!reader_running && !writer_running) {
    shutdown_socket();
    pipe_lock.Unlock();
    msgr->queue_reap(this);
  } else {
    pipe_lock.Unlock();
  }
}

static void alloc_aligned_buffer(bufferlist& data, unsigned len, unsigned off)
{
  // create a buffer to read into that matches the data alignment
  unsigned left = len;
  unsigned head = 0;
  if (off & ~CEPH_PAGE_MASK) {
    // head
    head = MIN(CEPH_PAGE_SIZE - (off & ~CEPH_PAGE_MASK), left);
    bufferptr bp = buffer::create(head);
    data.push_back(bp);
    left -= head;
  }
  unsigned middle = left & CEPH_PAGE_MASK;
  if (middle > 0) {
    bufferptr bp = buffer::create_page_aligned(middle);
    data.push_back(bp);
    left -= middle;
  }
  if (left) {
    bufferptr bp = buffer::create(left);
    data.push_back(bp);
  }
}

int SimpleMessenger::Pipe::read_message(Message **pm)
{
  int ret = -1;
  // envelope
  //ldout(msgr->cct,10) << "receiver.read_message from sd " << sd  << dendl;
  
  ceph_msg_header header; 
  ceph_msg_footer footer;
  __u32 header_crc;
  
  if (connection_state->has_feature(CEPH_FEATURE_NOSRCADDR)) {
    if (tcp_read(msgr->cct,  sd, (char*)&header, sizeof(header), msgr->timeout ) < 0)
      return -1;
    header_crc = ceph_crc32c_le(0, (unsigned char *)&header, sizeof(header) - sizeof(header.crc));
  } else {
    ceph_msg_header_old oldheader;
    if (tcp_read(msgr->cct,  sd, (char*)&oldheader, sizeof(oldheader), msgr->timeout ) < 0)
      return -1;
    // this is fugly
    memcpy(&header, &oldheader, sizeof(header));
    header.src = oldheader.src.name;
    header.reserved = oldheader.reserved;
    header.crc = oldheader.crc;
    header_crc = ceph_crc32c_le(0, (unsigned char *)&oldheader, sizeof(oldheader) - sizeof(oldheader.crc));
  }

  ldout(msgr->cct,20) << "reader got envelope type=" << header.type
           << " src " << entity_name_t(header.src)
           << " front=" << header.front_len
	   << " data=" << header.data_len
	   << " off " << header.data_off
           << dendl;

  // verify header crc
  if (header_crc != header.crc) {
    ldout(msgr->cct,0) << "reader got bad header crc " << header_crc << " != " << header.crc << dendl;
    return -1;
  }

  bufferlist front, middle, data;
  int front_len, middle_len;
  unsigned data_len, data_off;
  int aborted;
  Message *message;

  uint64_t message_size = header.front_len + header.middle_len + header.data_len;
  if (message_size) {
    if (policy.throttler) {
      ldout(msgr->cct,10) << "reader wants " << message_size << " from policy throttler "
	       << policy.throttler->get_current() << "/"
	       << policy.throttler->get_max() << dendl;
      policy.throttler->get(message_size);
    }

    // throttle total bytes waiting for dispatch.  do this _after_ the
    // policy throttle, as this one does not deadlock (unless dispatch
    // blocks indefinitely, which it shouldn't).  in contrast, the
    // policy throttle carries for the lifetime of the message.
    ldout(msgr->cct,10) << "reader wants " << message_size << " from dispatch throttler "
	     << msgr->dispatch_throttler.get_current() << "/"
	     << msgr->dispatch_throttler.get_max() << dendl;
    msgr->dispatch_throttler.get(message_size);
  }

  // read front
  front_len = header.front_len;
  if (front_len) {
    bufferptr bp = buffer::create(front_len);
    if (tcp_read(msgr->cct,  sd, bp.c_str(), front_len, msgr->timeout ) < 0)
      goto out_dethrottle;
    front.push_back(bp);
    ldout(msgr->cct,20) << "reader got front " << front.length() << dendl;
  }

  // read middle
  middle_len = header.middle_len;
  if (middle_len) {
    bufferptr bp = buffer::create(middle_len);
    if (tcp_read(msgr->cct,  sd, bp.c_str(), middle_len, msgr->timeout ) < 0)
      goto out_dethrottle;
    middle.push_back(bp);
    ldout(msgr->cct,20) << "reader got middle " << middle.length() << dendl;
  }


  // read data
  data_len = le32_to_cpu(header.data_len);
  data_off = le32_to_cpu(header.data_off);
  if (data_len) {
    unsigned offset = 0;
    unsigned left = data_len;

    bufferlist newbuf, rxbuf;
    bufferlist::iterator blp;
    int rxbuf_version = 0;
	
    while (left > 0) {
      // wait for data
      if (tcp_read_wait(sd, msgr->timeout) < 0)
	goto out_dethrottle;

      // get a buffer
      connection_state->lock.Lock();
      map<tid_t,pair<bufferlist,int> >::iterator p = connection_state->rx_buffers.find(header.tid);
      if (p != connection_state->rx_buffers.end()) {
	if (rxbuf.length() == 0 || p->second.second != rxbuf_version) {
	  ldout(msgr->cct,10) << "reader seleting rx buffer v " << p->second.second
		   << " at offset " << offset
		   << " len " << p->second.first.length() << dendl;
	  rxbuf = p->second.first;
	  rxbuf_version = p->second.second;
	  // make sure it's big enough
	  if (rxbuf.length() < data_len)
	    rxbuf.push_back(buffer::create(data_len - rxbuf.length()));
	  blp = p->second.first.begin();
	  blp.advance(offset);
	}
      } else {
	if (!newbuf.length()) {
	  ldout(msgr->cct,20) << "reader allocating new rx buffer at offset " << offset << dendl;
	  alloc_aligned_buffer(newbuf, data_len, data_off);
	  blp = newbuf.begin();
	  blp.advance(offset);
	}
      }
      bufferptr bp = blp.get_current_ptr();
      int read = MIN(bp.length(), left);
      ldout(msgr->cct,20) << "reader reading nonblocking into " << (void*)bp.c_str() << " len " << bp.length() << dendl;
      int got = tcp_read_nonblocking(msgr->cct, sd, bp.c_str(), read);
      ldout(msgr->cct,30) << "reader read " << got << " of " << read << dendl;
      connection_state->lock.Unlock();
      if (got < 0)
	goto out_dethrottle;
      if (got > 0) {
	blp.advance(got);
	data.append(bp, 0, got);
	offset += got;
	left -= got;
      } // else we got a signal or something; just loop.
    }
  }

  // footer
  if (tcp_read(msgr->cct, sd, (char*)&footer, sizeof(footer), msgr->timeout) < 0)
    goto out_dethrottle;
  
  aborted = (footer.flags & CEPH_MSG_FOOTER_COMPLETE) == 0;
  ldout(msgr->cct,10) << "aborted = " << aborted << dendl;
  if (aborted) {
    ldout(msgr->cct,0) << "reader got " << front.length() << " + " << middle.length() << " + " << data.length()
	    << " byte message.. ABORTED" << dendl;
    ret = 0;
    goto out_dethrottle;
  }

  ldout(msgr->cct,20) << "reader got " << front.length() << " + " << middle.length() << " + " << data.length()
	   << " byte message" << dendl;
  message = decode_message(msgr->cct, header, footer, front, middle, data);
  if (!message) {
    ret = -EINVAL;
    goto out_dethrottle;
  }

  message->set_throttler(policy.throttler);

  // store reservation size in message, so we don't get confused
  // by messages entering the dispatch queue through other paths.
  message->set_dispatch_throttle_size(message_size);

  *pm = message;
  return 0;

 out_dethrottle:
  // release bytes reserved from the throttlers on failure
  if (message_size) {
    if (policy.throttler) {
      ldout(msgr->cct,10) << "reader releasing " << message_size << " to policy throttler "
	       << policy.throttler->get_current() << "/"
	       << policy.throttler->get_max() << dendl;
      policy.throttler->put(message_size);
    }

    msgr->dispatch_throttle_release(message_size);
  }
  return ret;
}

int SimpleMessenger::Pipe::do_sendmsg(int sd, struct msghdr *msg, int len, bool more)
{
  char buf[80];

  while (len > 0) {
    if (0) { // sanity
      int l = 0;
      for (unsigned i=0; i<msg->msg_iovlen; i++)
	l += msg->msg_iov[i].iov_len;
      assert(l == len);
    }

    int r = ::sendmsg(sd, msg, MSG_NOSIGNAL | (more ? MSG_MORE : 0));
    if (r == 0) 
      ldout(msgr->cct,10) << "do_sendmsg hmm do_sendmsg got r==0!" << dendl;
    if (r < 0) { 
      ldout(msgr->cct,1) << "do_sendmsg error " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      return -1;
    }
    if (state == STATE_CLOSED) {
      ldout(msgr->cct,10) << "do_sendmsg oh look, state == CLOSED, giving up" << dendl;
      errno = EINTR;
      return -1; // close enough
    }

    if (0) {
      // hex dump
      struct iovec *v = msg->msg_iov;
      size_t left = r;
      size_t vpos = 0;
      ldout(msgr->cct,0) << "do_sendmsg wrote " << r << " bytes, hexdump:\n";
      int pos = 0;
      int col = 0;
      char buf[20];
      while (left > 0) {
	if (col == 0) {
	  snprintf(buf, sizeof(buf), "%05x : ", pos);
	  *_dout << buf;
	}
	snprintf(buf, sizeof(buf), " %02x", ((unsigned char*)v->iov_base)[vpos]);
	*_dout << buf;
	left--;
	if (!left)
	  break;
	vpos++;
	pos++;
	if (vpos == v->iov_len) {
	  v++;
	  vpos = 0;
	}	  
	col++;
	if (col == 16) {
	  *_dout << "\n";
	  col = 0;
	}
      }
      *_dout << dendl;
    }

    len -= r;
    if (len == 0) break;
    
    // hrmph.  trim r bytes off the front of our message.
    ldout(msgr->cct,20) << "do_sendmsg short write did " << r << ", still have " << len << dendl;
    while (r > 0) {
      if (msg->msg_iov[0].iov_len <= (size_t)r) {
	// lose this whole item
	//ldout(msgr->cct,30) << "skipping " << msg->msg_iov[0].iov_len << ", " << (msg->msg_iovlen-1) << " v, " << r << " left" << dendl;
	r -= msg->msg_iov[0].iov_len;
	msg->msg_iov++;
	msg->msg_iovlen--;
      } else {
	// partial!
	//ldout(msgr->cct,30) << "adjusting " << msg->msg_iov[0].iov_len << ", " << msg->msg_iovlen << " v, " << r << " left" << dendl;
	msg->msg_iov[0].iov_base = (char *)msg->msg_iov[0].iov_base + r;
	msg->msg_iov[0].iov_len -= r;
	break;
      }
    }
  }
  return 0;
}


int SimpleMessenger::Pipe::write_ack(uint64_t seq)
{
  ldout(msgr->cct,10) << "write_ack " << seq << dendl;

  char c = CEPH_MSGR_TAG_ACK;
  ceph_le64 s;
  s = seq;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec msgvec[2];
  msgvec[0].iov_base = &c;
  msgvec[0].iov_len = 1;
  msgvec[1].iov_base = &s;
  msgvec[1].iov_len = sizeof(s);
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 2;
  
  if (do_sendmsg(sd, &msg, 1 + sizeof(s), true) < 0) 
    return -1;	
  return 0;
}

int SimpleMessenger::Pipe::write_keepalive()
{
  ldout(msgr->cct,10) << "write_keepalive" << dendl;

  char c = CEPH_MSGR_TAG_KEEPALIVE;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec msgvec[2];
  msgvec[0].iov_base = &c;
  msgvec[0].iov_len = 1;
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 1;
  
  if (do_sendmsg(sd, &msg, 1) < 0) 
    return -1;	
  return 0;
}


int SimpleMessenger::Pipe::write_message(Message *m)
{
  ceph_msg_header& header = m->get_header();
  ceph_msg_footer& footer = m->get_footer();
  int ret;

  // get envelope, buffers
  header.front_len = m->get_payload().length();
  header.middle_len = m->get_middle().length();
  header.data_len = m->get_data().length();
  footer.flags = CEPH_MSG_FOOTER_COMPLETE;
  m->calc_header_crc();

  bufferlist blist = m->get_payload();
  blist.append(m->get_middle());
  blist.append(m->get_data());
  
  ldout(msgr->cct,20)  << "write_message " << m << dendl;
  
  // set up msghdr and iovecs
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec *msgvec = new iovec[3 + blist.buffers().size()];  // conservative upper bound
  msg.msg_iov = msgvec;
  int msglen = 0;
  
  // send tag
  char tag = CEPH_MSGR_TAG_MSG;
  msgvec[msg.msg_iovlen].iov_base = &tag;
  msgvec[msg.msg_iovlen].iov_len = 1;
  msglen++;
  msg.msg_iovlen++;

  // send envelope
  ceph_msg_header_old oldheader;
  if (connection_state->has_feature(CEPH_FEATURE_NOSRCADDR)) {
    msgvec[msg.msg_iovlen].iov_base = (char*)&header;
    msgvec[msg.msg_iovlen].iov_len = sizeof(header);
    msglen += sizeof(header);
    msg.msg_iovlen++;
  } else {
    memcpy(&oldheader, &header, sizeof(header));
    oldheader.src.name = header.src;
    oldheader.src.addr = connection_state->get_peer_addr();
    oldheader.orig_src = oldheader.src;
    oldheader.reserved = header.reserved;
    oldheader.crc = ceph_crc32c_le(0, (unsigned char*)&oldheader,
			      sizeof(oldheader) - sizeof(oldheader.crc));
    msgvec[msg.msg_iovlen].iov_base = (char*)&oldheader;
    msgvec[msg.msg_iovlen].iov_len = sizeof(oldheader);
    msglen += sizeof(oldheader);
    msg.msg_iovlen++;
  }

  // payload (front+data)
  list<bufferptr>::const_iterator pb = blist.buffers().begin();
  int b_off = 0;  // carry-over buffer offset, if any
  int bl_pos = 0; // blist pos
  int left = blist.length();

  while (left > 0) {
    int donow = MIN(left, (int)pb->length()-b_off);
    if (donow == 0) {
      ldout(msgr->cct,0) << "donow = " << donow << " left " << left << " pb->length " << pb->length()
	      << " b_off " << b_off << dendl;
    }
    assert(donow > 0);
    ldout(msgr->cct,30) << " bl_pos " << bl_pos << " b_off " << b_off
	     << " leftinchunk " << left
	     << " buffer len " << pb->length()
	     << " writing " << donow 
	     << dendl;
    
    if (msg.msg_iovlen >= IOV_MAX-2) {
      if (do_sendmsg(sd, &msg, msglen, true)) 
	goto fail;
      
      // and restart the iov
      msg.msg_iov = msgvec;
      msg.msg_iovlen = 0;
      msglen = 0;
    }
    
    msgvec[msg.msg_iovlen].iov_base = (void*)(pb->c_str()+b_off);
    msgvec[msg.msg_iovlen].iov_len = donow;
    msglen += donow;
    msg.msg_iovlen++;
    
    left -= donow;
    assert(left >= 0);
    b_off += donow;
    bl_pos += donow;
    if (left == 0)
      break;
    while (b_off == (int)pb->length()) {
      pb++;
      b_off = 0;
    }
  }
  assert(left == 0);

  // send footer
  msgvec[msg.msg_iovlen].iov_base = (void*)&footer;
  msgvec[msg.msg_iovlen].iov_len = sizeof(footer);
  msglen += sizeof(footer);
  msg.msg_iovlen++;

  // send
  if (do_sendmsg(sd, &msg, msglen)) 
    goto fail;

  ret = 0;

 out:
  delete[] msgvec;
  return ret;

 fail:
  ret = -1;
  goto out;
}


/********************************************
 * SimpleMessenger
 */
#undef dout_prefix
#define dout_prefix _prefix(_dout, this)

void SimpleMessenger::dispatch_throttle_release(uint64_t msize)
{
  if (msize) {
    ldout(cct,10) << "dispatch_throttle_release " << msize << " to dispatch throttler "
	    << msgr->dispatch_throttler.get_current() << "/"
	    << msgr->dispatch_throttler.get_max() << dendl;
    dispatch_throttler.put(msize);
  }
}

void SimpleMessenger::reaper_entry()
{
  ldout(cct,10) << "reaper_entry start" << dendl;
  lock.Lock();
  while (!reaper_stop) {
    reaper();
    reaper_cond.Wait(lock);
  }
  lock.Unlock();
  ldout(cct,10) << "reaper_entry done" << dendl;
}

/*
 * note: assumes lock is held
 */
void SimpleMessenger::reaper()
{
  ldout(cct,10) << "reaper" << dendl;
  assert(lock.is_locked());

  while (!pipe_reap_queue.empty()) {
    Pipe *p = pipe_reap_queue.front();
    pipe_reap_queue.pop_front();
    ldout(cct,10) << "reaper reaping pipe " << p << " " << p->get_peer_addr() << dendl;
    p->pipe_lock.Lock();
    p->discard_queue();
    p->pipe_lock.Unlock();
    p->unregister_pipe();
    assert(pipes.count(p));
    pipes.erase(p);
    p->join();
    if (p->sd >= 0)
      ::close(p->sd);
    ldout(cct,10) << "reaper reaped pipe " << p << " " << p->get_peer_addr() << dendl;
    if (p->connection_state)
      p->connection_state->clear_pipe();
    p->put();
    ldout(cct,10) << "reaper deleted pipe " << p << dendl;
  }
  ldout(cct,10) << "reaper done" << dendl;
}

void SimpleMessenger::queue_reap(Pipe *pipe)
{
  ldout(cct,10) << "queue_reap " << pipe << dendl;
  lock.Lock();
  pipe_reap_queue.push_back(pipe);
  reaper_cond.Signal();
  lock.Unlock();
}



int SimpleMessenger::bind(entity_addr_t bind_addr, int64_t nonce)
{
  lock.Lock();
  if (started) {
    ldout(cct,10) << "rank.bind already started" << dendl;
    lock.Unlock();
    return -1;
  }
  ldout(cct,10) << "rank.bind " << bind_addr << dendl;
  lock.Unlock();

  // bind to a socket
  return accepter.bind(nonce, bind_addr);
}

int SimpleMessenger::rebind(int avoid_port)
{
  ldout(cct,1) << "rebind avoid " << avoid_port << dendl;
  mark_down_all();
  return accepter.rebind(avoid_port);
}

int SimpleMessenger::start_with_nonce(uint64_t nonce)
{
  lock.Lock();
  ldout(cct,1) << "messenger.start" << dendl;

  // register at least one entity, first!
  assert(my_type >= 0); 

  assert(!started);
  started = true;

  if (!did_bind)
    ms_addr.nonce = nonce;

  lock.Unlock();

  if (did_bind)
    accepter.start();

  reaper_started = true;
  reaper_thread.create();
  return 0;
}


/* connect_rank
 * NOTE: assumes messenger.lock held.
 */
SimpleMessenger::Pipe *SimpleMessenger::connect_rank(const entity_addr_t& addr, int type)
{
  assert(lock.is_locked());
  assert(addr != ms_addr);
  
  ldout(cct,10) << "connect_rank to " << addr << ", creating pipe and registering" << dendl;
  
  // create pipe
  Pipe *pipe = new Pipe(this, Pipe::STATE_CONNECTING);
  pipe->pipe_lock.Lock();
  pipe->set_peer_type(type);
  pipe->set_peer_addr(addr);
  pipe->policy = get_policy(type);
  pipe->start_writer();
  pipe->pipe_lock.Unlock();
  pipe->register_pipe();
  pipes.insert(pipe);

  return pipe;
}






AuthAuthorizer *SimpleMessenger::get_authorizer(int peer_type, bool force_new)
{
  return ms_deliver_get_authorizer(peer_type, force_new);
}

bool SimpleMessenger::verify_authorizer(Connection *con, int peer_type,
					int protocol, bufferlist& authorizer, bufferlist& authorizer_reply,
					bool& isvalid)
{
  return ms_deliver_verify_authorizer(con, peer_type, protocol, authorizer, authorizer_reply, isvalid);
}



/* register_entity 
 */
bool SimpleMessenger::register_entity(entity_name_t name)
{
  ldout(cct,10) << "register_entity " << name << dendl;
  lock.Lock();
  
  if (!destination_stopped) { //already have a working entity set
    lock.Unlock();
    return false;
  }

  // set it up
  Messenger::set_myname(name);
  // now i know my type.
  if (my_type >= 0)
    assert(my_type == name.type());
  else
    my_type = name.type();

  destination_stopped = false;

  ldout(cct,10) << "register_entity " << name << " at " << get_myaddr() << dendl;

  msgr->init_local_pipe();

  lock.Unlock();
  return true;
}

void SimpleMessenger::submit_message(Message *m, Pipe *pipe)
{ 
  lock.Lock();
  if (pipe == dispatch_queue.local_pipe) {
    ldout(cct,20) << "submit_message " << *m << " local" << dendl;
    dispatch_queue.local_delivery(m, m->get_priority());
  } else {
    pipe->pipe_lock.Lock();
    if (pipe->state == Pipe::STATE_CLOSED) {
      ldout(cct,20) << "submit_message " << *m << " ignoring closed pipe " << pipe->peer_addr << dendl;
      pipe->unregister_pipe();
      pipe->pipe_lock.Unlock();
      m->put();
    } else {
      ldout(cct,20) << "submit_message " << *m << " remote " << pipe->peer_addr << dendl;
      pipe->_send(m);
      pipe->pipe_lock.Unlock();
    }
  }
  lock.Unlock();
}

Connection *SimpleMessenger::get_connection(const entity_inst_t& dest)
{
  Mutex::Locker l(lock);
  Pipe *pipe = NULL;
  if (ms_addr == dest.addr) {
    // local
    pipe = dispatch_queue.local_pipe;
  } else {
    // remote
    hash_map<entity_addr_t, Pipe*>::iterator p = rank_pipe.find(dest.addr);
    if (p != rank_pipe.end()) {
      pipe = p->second;
      pipe->pipe_lock.Lock();
      if (pipe->state == Pipe::STATE_CLOSED) {
	pipe->unregister_pipe();
	pipe->pipe_lock.Unlock();
	pipe = 0;
      } else {
	pipe->pipe_lock.Unlock();
      }
    }
    if (!pipe) {
      Policy& policy = get_policy(dest.name.type());
      if (policy.lossy && policy.server)
	pipe = NULL;
      else
	pipe = connect_rank(dest.addr, dest.name.type());
    }
  }
  if (pipe)
    return (Connection *)pipe->connection_state->get();
  return NULL;
}


void SimpleMessenger::submit_message(Message *m, const entity_addr_t& dest_addr, int dest_type, bool lazy)
{
  // this is just to make sure that a changeset is working properly;
  // if you start using the refcounting more and have multiple people
  // hanging on to a message, ditch the assert!
  assert(m->nref.read() == 1);

  if (dest_addr == entity_addr_t()) {
    ldout(cct,0) << "submit_message message " << *m << " with empty dest " << dest_addr << dendl;
    m->put();
    return;
  }

  lock.Lock();
  {
    // local?
    if (ms_addr == dest_addr) {
      if (!destination_stopped) {
        // local
        ldout(cct,20) << "submit_message " << *m << " local" << dendl;
	dispatch_queue.local_delivery(m, m->get_priority());
      } else {
        ldout(cct,0) << "submit_message " << *m << " " << dest_addr << " local but no local endpoint, dropping." << dendl;
        assert(0);  // hmpf, this is probably mds->mon beacon from newsyn.
        m->put();
      }
    } else {
      // remote pipe.
      Pipe *pipe = 0;
      if (rank_pipe.count(dest_addr)) {
	pipe = rank_pipe[ dest_addr ];
	pipe->pipe_lock.Lock();
	if (pipe->state == Pipe::STATE_CLOSED) {
	  ldout(cct,20) << "submit_message " << *m << " remote, " << dest_addr << ", ignoring closed pipe." << dendl;
	  pipe->unregister_pipe();
	  pipe->pipe_lock.Unlock();
	  pipe = 0;
	} else {
	  ldout(cct,20) << "submit_message " << *m << " remote, " << dest_addr << ", have pipe." << dendl;
	  
	  pipe->_send(m);
	  pipe->pipe_lock.Unlock();
	}
      }
      if (!pipe) {
	Policy& policy = get_policy(dest_type);
	if (policy.lossy && policy.server) {
	  ldout(cct,20) << "submit_message " << *m << " remote, " << dest_addr << ", lossy server for target type "
		   << ceph_entity_type_name(dest_type) << ", no session, dropping." << dendl;
	  m->put();
	} else if (lazy) {
	  ldout(cct,20) << "submit_message " << *m << " remote, " << dest_addr << ", lazy, dropping." << dendl;
	  m->put();
	} else {
	  ldout(cct,20) << "submit_message " << *m << " remote, " << dest_addr << ", new pipe." << dendl;
	  // not connected.
	  pipe = connect_rank(dest_addr, dest_type);
	  pipe->send(m);
	}
      }
    }
  }

  lock.Unlock();
}

int SimpleMessenger::send_keepalive(const entity_inst_t& dest)
{
  const entity_addr_t dest_addr = dest.addr;
  entity_addr_t dest_proc_addr = dest_addr;

  lock.Lock();
  {
    // local?
    if (ms_addr != dest_addr) {
      // remote.
      Pipe *pipe = 0;
      if (rank_pipe.count( dest_proc_addr )) {
        // connected?
        pipe = rank_pipe[ dest_proc_addr ];
	pipe->pipe_lock.Lock();
	if (pipe->state == Pipe::STATE_CLOSED) {
	  ldout(cct,20) << "send_keepalive remote, " << dest_addr << ", ignoring old closed pipe." << dendl;
	  pipe->unregister_pipe();
	  pipe->pipe_lock.Unlock();
	  pipe = 0;
	} else {
	  ldout(cct,20) << "send_keepalive remote, " << dest_addr << ", have pipe." << dendl;
	  pipe->_send_keepalive();
	  pipe->pipe_lock.Unlock();
	}
      }
      if (!pipe)
	ldout(cct,20) << "send_keepalive no pipe for " << dest_addr << ", doing nothing." << dendl;
    }
  }
  lock.Unlock();
  return 0;
}

int SimpleMessenger::send_keepalive(Connection *con)
{
  SimpleMessenger::Pipe *pipe = (SimpleMessenger::Pipe *)con->get_pipe();
  if (pipe) {
    ldout(cct,20) << "send_keepalive con " << con << ", have pipe." << dendl;
    pipe->pipe_lock.Lock();
    pipe->_send_keepalive();
    pipe->pipe_lock.Unlock();
    pipe->put();
  } else {
    ldout(cct,0) << "send_keepalive con " << con << ", no pipe." << dendl;
  }
  return 0;
}



void SimpleMessenger::wait()
{
  lock.Lock();
  if (!started) {
    lock.Unlock();
    return;
  }
  while (!destination_stopped) {
    ldout(cct,10) << "wait: still active" << dendl;
    wait_cond.Wait(lock);
    ldout(cct,10) << "wait: woke up" << dendl;
  }
  ldout(cct,10) << "wait: everything stopped" << dendl;
  lock.Unlock();
  
  // done!  clean up.
  if (did_bind) {
    ldout(cct,20) << "wait: stopping accepter thread" << dendl;
    accepter.stop();
    ldout(cct,20) << "wait: stopped accepter thread" << dendl;
  }

  if (reaper_started) {
    ldout(cct,20) << "wait: stopping reaper thread" << dendl;
    lock.Lock();
    reaper_cond.Signal();
    reaper_stop = true;
    lock.Unlock();
    reaper_thread.join();
    reaper_started = false;
    ldout(cct,20) << "wait: stopped reaper thread" << dendl;
  }

  // close+reap all pipes
  lock.Lock();
  {
    ldout(cct,10) << "wait: closing pipes" << dendl;

    while (!rank_pipe.empty()) {
      Pipe *p = rank_pipe.begin()->second;
      p->unregister_pipe();
      p->pipe_lock.Lock();
      p->stop();
      p->pipe_lock.Unlock();
    }

    reaper();
    ldout(cct,10) << "wait: waiting for pipes " << pipes << " to close" << dendl;
    while (!pipes.empty()) {
      reaper_cond.Wait(lock);
      reaper();
    }
  }
  lock.Unlock();

  ldout(cct,10) << "wait: done." << dendl;
  ldout(cct,1) << "shutdown complete." << dendl;
  started = false;
  my_type = -1;
}


void SimpleMessenger::mark_down_all()
{
  ldout(cct,1) << "mark_down_all" << dendl;
  lock.Lock();
  while (!rank_pipe.empty()) {
    hash_map<entity_addr_t,Pipe*>::iterator it = rank_pipe.begin();
    Pipe *p = it->second;
    ldout(cct,5) << "mark_down_all " << it->first << " " << p << dendl;
    rank_pipe.erase(it);
    p->unregister_pipe();
    p->pipe_lock.Lock();
    p->stop();
    p->pipe_lock.Unlock();
  }
  lock.Unlock();
}

void SimpleMessenger::mark_down(const entity_addr_t& addr)
{
  lock.Lock();
  if (rank_pipe.count(addr)) {
    Pipe *p = rank_pipe[addr];
    ldout(cct,1) << "mark_down " << addr << " -- " << p << dendl;
    p->unregister_pipe();
    p->pipe_lock.Lock();
    p->stop();
    p->pipe_lock.Unlock();
  } else {
    ldout(cct,1) << "mark_down " << addr << " -- pipe dne" << dendl;
  }
  lock.Unlock();
}

void SimpleMessenger::mark_down(Connection *con)
{
  lock.Lock();
  Pipe *p = (Pipe *)con->get_pipe();
  if (p) {
    ldout(cct,1) << "mark_down " << con << " -- " << p << dendl;
    p->unregister_pipe();
    p->pipe_lock.Lock();
    p->stop();
    p->pipe_lock.Unlock();
    p->put();
  } else {
    ldout(cct,1) << "mark_down " << con << " -- pipe dne" << dendl;
  }
  lock.Unlock();
}

void SimpleMessenger::mark_down_on_empty(Connection *con)
{
  lock.Lock();
  Pipe *p = (Pipe *)con->get_pipe();
  if (p) {
    p->pipe_lock.Lock();
    p->unregister_pipe();
    if (p->out_q.empty()) {
      ldout(cct,1) << "mark_down_on_empty " << con << " -- " << p << " closing (queue is empty)" << dendl;
      p->stop();
    } else {
      ldout(cct,1) << "mark_down_on_empty " << con << " -- " << p << " marking (queue is not empty)" << dendl;
      p->close_on_empty = true;
    }
    p->pipe_lock.Unlock();
    p->put();
  } else {
    ldout(cct,1) << "mark_down_on_empty " << con << " -- pipe dne" << dendl;
  }
  lock.Unlock();
}

void SimpleMessenger::mark_disposable(Connection *con)
{
  lock.Lock();
  Pipe *p = (Pipe *)con->get_pipe();
  if (p) {
    ldout(cct,1) << "mark_disposable " << con << " -- " << p << dendl;
    p->pipe_lock.Lock();
    p->policy.lossy = true;
    p->disposable = true;
    p->pipe_lock.Unlock();
    p->put();
  } else {
    ldout(cct,1) << "mark_disposable " << con << " -- pipe dne" << dendl;
  }
  lock.Unlock();
}

void SimpleMessenger::learned_addr(const entity_addr_t &peer_addr_for_me)
{
  lock.Lock();
  int port = ms_addr.get_port();
  ms_addr.addr = peer_addr_for_me.addr;
  ms_addr.set_port(port);
  ldout(cct,1) << "learned my addr " << ms_addr << dendl;
  need_addr = false;
  init_local_pipe();
  lock.Unlock();
}

void SimpleMessenger::init_local_pipe()
{
  dispatch_queue.local_pipe->connection_state->peer_addr = msgr->ms_addr;
  dispatch_queue.local_pipe->connection_state->peer_type = msgr->my_type;
}
