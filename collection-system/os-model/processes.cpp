/**
 * Copyright 2020 IBM
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../os-model/processes.h"

#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "logger.h"

static const std::string EPOCH_TIME_UTC = "1970-01-01 00:00:00.000";
static const std::string FUTURE_TIME_UTC = "9999-01-01 00:00:00.000";

/*------------------------------
 * Helpers
 *------------------------------*/

/**
 * Converts a string representing a hex
 * number to an integer (in base-10).
 */
int hex_to_dec(std::string hex_string) {
  int dec_num;
  std::stringstream ss;
  ss << std::hex << hex_string;
  ss >> dec_num;
  return dec_num;
}

/*------------------------------
 * ProcessTable
 *------------------------------*/

ProcessTable::~ProcessTable() {
  // clean up processes
  for (live_proc_map::iterator itr = live_processes.begin(); itr != live_processes.end(); itr++) {
    delete itr->second;
  }
  for (ProcessEvent *e : dead_processes) {
    delete e;
  }
  // clean up process groups
  for (live_pg_map::iterator itr = live_process_groups.begin();
      itr != live_process_groups.end(); itr++) {
    itr->second->make_dead("1970-01-01 00:00:00.000");
    delete itr->second;
  }
  for (ProcessGroupEvent*e :  dead_process_groups) {
    delete e;
  }
  // clean up IPC
  for (IPCEvent *e: finished_ipcs) {
    delete e;
  }
  // clean up sockets
  for (SocketEvent *e: finished_sockets) {
    delete e;
  }
  // clean up socket connections
  for (SocketConnectEvent *e: finished_socket_connects) {
    delete e;
  }
}

osm_rc_t ProcessTable::apply_syscall(SyscallEvent *se) {
  // Create the calling process if we have not seen it before.
  // If we haven't seen the corresponding clone() call, we don't know
  // whether the parent is a thread or not and hence just create a new process.
  LiveProcess *caller = add_caller_if_unseen(se);
  LOGGER_LOG_DEBUG("process " << std::to_string(caller->pid) << " made syscall " << se->syscall_name);

  switch (string_to_syscall[se->syscall_name]) {
  // process-related syscalls
  case osm_syscall_clone:
    clone(se);
    break;
  case osm_syscall_vfork:
    vfork(se);
    break;
  case osm_syscall_execve:
    execve(se);
    break;
  case osm_syscall_setpgid:
    setpgid(se);
    break;
  // TODO These have different effects and will lead to "zombie processes" (undetected process exits).
  case osm_syscall_exit:
    //exit(se);
    break;
  case osm_syscall_exit_group:
    exit_group(se);
    break;
  case osm_syscall_pipe:
    pipe(se);
    break;
  case osm_syscall_close:
    close(se);
    break;
  case osm_syscall_dup2:
    dup2(se);
    break;
  case osm_syscall_socket:
    socket(se);
    break;
  case osm_syscall_connect:
    connect(se);
    break;
  case osm_syscall_bind:
    bind(se);
    break;
  default:
    // Unmodeled syscalls should be discarded safely by OSModel::applySyscall.
    LOGGER_LOG_ERROR("Unmodeled syscall: " << se->serialize());
    break;
  }

  return osm_rc_ok;
}

std::vector<Event*> ProcessTable::reap_os_events() {
  std::vector<Event*> ret;
  size_t size = dead_processes.size() + dead_process_groups.size() + finished_ipcs.size()
      + finished_sockets.size() + finished_socket_connects.size();
  ret.reserve(size);

  // process events
  ret.insert(ret.end(), dead_processes.begin(), dead_processes.end());
  dead_processes.clear();

  // process group events
  ret.insert(ret.end(), dead_process_groups.begin(), dead_process_groups.end());
  dead_process_groups.clear();

  // IPC events
  ret.insert(ret.end(), finished_ipcs.begin(), finished_ipcs.end());
  finished_ipcs.clear();

  // socket events
  ret.insert(ret.end(), finished_sockets.begin(), finished_sockets.end());
  finished_sockets.clear();

  // socket connect events
  ret.insert(ret.end(), finished_socket_connects.begin(), finished_socket_connects.end());
  finished_socket_connects.clear();

  return ret;
}

LiveProcess* ProcessTable::get_live_process(osm_pid_t pid) {
  LiveProcess *lp = nullptr;
  if (live_processes.find(pid) != live_processes.end()) {
    lp = live_processes[pid];
  }
  return lp;
}

LiveProcessGroup* ProcessTable::get_live_process_group(osm_pgid_t pgid) {
  LiveProcessGroup *lpg = nullptr;
  if (live_process_groups.find(pgid) != live_process_groups.end()) {
    lpg = live_process_groups[pgid];
  }
  return lpg;
}

LiveProcess* ProcessTable::add_caller_if_unseen(const SyscallEvent *se) {
  LiveProcess *lp = get_live_process(se->pid);
  if (!lp) {
    // create and add to table
    lp = new LiveProcess(se);
    register_live_process(lp);
  }
  return lp;
}

LiveProcess* ProcessTable::add_process_if_unseen(osm_pid_t pid) {
  LiveProcess *lp = get_live_process(pid);
  if (!lp) {
    // create and add to table
    lp = new LiveProcess(pid);
    register_live_process(lp);
  }
  return lp;
}

void ProcessTable::register_live_process(LiveProcess *lp) {
  assert(lp);
  assert(!get_live_process(lp->pid));

  LOGGER_LOG_DEBUG("Adding lp " << lp->pid << "(" << lp << ")" << " to liveProcesses");
  live_processes[lp->pid] = lp;
}

void ProcessTable::register_live_process_group(LiveProcessGroup *lpg) {
  assert(lpg);
  assert(!get_live_process_group(lpg->pgid));
  assert(lpg->is_empty());

  LOGGER_LOG_DEBUG("Adding lpg " << lpg->pgid << "(" << lpg << ")" << " to liveProcessGroups");
  live_process_groups[lpg->pgid] = lpg;
}

void ProcessTable::clone(SyscallEvent *se) {
  // get the child pid, stored in the return code
  osm_pid_t child_pid = se->rc;

  // It might already be live (if so, mark it dead). This is possible if the exit
  // event was dropped, e.g. because the process died abnormally instead of calling
  // exit (e.g. segfault).
  LiveProcess *old_process = get_live_process(child_pid);
  if (old_process) {
    LOGGER_LOG_INFO("ProcessTable::clone: Found still-live process in new pid " << child_pid
        << ", making it dead at time " << se->event_time.c_str());
    finalize_process(old_process, se->event_time);
    old_process = nullptr;
  }

  // we know the parent process exists because we call 'add_caller_if_unseen' before
  // applying the syscall
  LiveProcess *parent = get_live_process(se->pid);
  assert(parent);

  // Check whether the cloned object is a thread or process. We do this by looking
  // at the flags and check whether CLONE_THREAD has been set. The flags are stored
  // in the first argument on x86 and Power (see the NOTES section at
  // http://man7.org/linux/man-pages/man2/clone.2.html for argument orders on
  // different architectures).
  //
  // TODO Is 'CLONE_THREAD' enough or do we need other flags?
  // TODO We should store the raw arguments in SyscallEvent so we can avoid the string comparison
  if (se->arg0.find("CLONE_THREAD") != std::string::npos) {
    // threads can clone new threads so we have to find the actual process parent
    while (parent->is_thread) {
      parent = ((LiveThread*) parent)->parent;
    }
    LOGGER_LOG_DEBUG("A thread has been cloned with tid " << std::to_string(child_pid) << " and parent "
        << std::to_string(parent->pid));

    LiveThread *new_thread = new LiveThread(parent, child_pid, se->event_time);
    register_live_process(new_thread);
    parent->threads[new_thread->pid] = new_thread;
    // we don't add threads to existing process groups
  } else {
    // threads can clone new processes so we have to find the actual process parent
    while (parent->is_thread) {
      parent = ((LiveThread*) parent)->parent;
    }
    LOGGER_LOG_DEBUG("A process has been cloned with pid " << std::to_string(child_pid));

    LiveProcess *new_process = new LiveProcess(parent, child_pid, se->event_time);
    register_live_process(new_process);
    // new_process inherited the process group, so if it's not prehistoric we can add it.
    try_to_add_process_to_process_group(new_process, se->event_time);
  }
}

void ProcessTable::vfork(SyscallEvent *se) {
  // get the child pid, stored in the return code
  osm_pid_t child_pid = se->rc;

  // we know the parent process exists because we call 'add_caller_if_unseen' before
  // applying the syscall
  LiveProcess *parent = get_live_process(se->pid);
  assert(parent);

  // It might already be live. If so, augment it with the information from the vfork call.
  //
  // This is a bit hacky but due to the fact that vfork is highly efficient and
  // hence, the corresponding vfork event may be emitted with the same timestamp
  // as the vforked process' execve call. As auditd events may arrive out of
  // order (https://slack.engineering/syscall-auditing-at-scale-e6a3ca8ac1b8)
  // there's a possibility that we see the execve events before the vfork and
  // as a result, may already have an oldProcess registered. Here, we assume that
  // if we see an oldProcess, it's due to the fact that we got an execve before the
  // actual vfork.
  //
  // TODO check if this has been fixed by the event ordering fix in auditd 2.8.6
  LiveProcess *old_process = get_live_process(child_pid);
  if (old_process) {
    LOGGER_LOG_DEBUG("ProcessTable::vfork: Found still-live process in new pid "
        << child_pid << ", augmenting it");
    // find parent process (not thread)
    while (parent->is_thread) {
      parent = ((LiveThread*) parent)->parent;
    }

    old_process->vfork(se->event_time, parent->pid, parent->pgid);
    try_to_add_process_to_process_group(old_process, se->event_time);
  } else {
    // find parent process (not thread)
    while (parent->is_thread) {
      parent = ((LiveThread*) parent)->parent;
    }

    LiveProcess *new_process = new LiveProcess(parent, child_pid, se->event_time);
    register_live_process(new_process);
    try_to_add_process_to_process_group(new_process, se->event_time);
  }
  LOGGER_LOG_DEBUG("A process has been cloned with pid " << std::to_string(child_pid) << " by "
      << std::to_string(se->pid));
}

void ProcessTable::pipe(SyscallEvent *se) {
  // we need to add the file descriptors describing both ends of the pipe to the
  // process (see SyscallEvent::data for the interpretation on pipe)
  std::shared_ptr<OpenFile> pipe = std::make_shared<Pipe>();
  int fd_read_int = std::stoi(se->data[0]);
  int fd_write_int = std::stoi(se->data[1]);
  FileDescriptor fd0(osm_fd_pipe_read, fd_read_int, pipe);
  FileDescriptor fd1(osm_fd_pipe_write, fd_write_int, pipe);

  // get the corresponding process
  LiveProcess *lp = get_live_process(se->pid);
  lp->fds[fd_read_int] = fd0;
  lp->fds[fd_write_int] = fd1;

  LOGGER_LOG_DEBUG("[" << lp->pid << "] Added file descriptor " << lp->fds[fd_read_int].str()
      << " to lp " << lp->pid << " and file descriptor " << lp->fds[fd_write_int].str()
      << " to lp " << lp->pid << ". Process now has " << lp->fds.size()
      << " open file descriptors.");
}

void ProcessTable::close(SyscallEvent *se) {
  // get the closed file descriptor (provided as hex number)
  int fd = hex_to_dec(se->arg0);

  LiveProcess *lp = get_live_process(se->pid);
  if (lp->fds.find(fd) == lp->fds.end()) {
    // we haven't seen this file descriptor being opened through one
    // of the system calls we're tracking so we just ignore that close
    return;
  }

  // Check if this is the last fd pointing to the target file and the file
  // is a pipe. In that case, finish the pipe and collect the IPC event.
  if (lp->fds[fd].get_target_file_references() == 1
      && lp->fds[fd].get_target_file_type() == OpenFile::OS_FILE_TYPE_PIPE) {
    IPCEvent *ev = ((Pipe*) lp->fds[fd].get_target_file())->to_ipc_event();
    // only add complete pipes to the list of IPC events
    if (ev) {
      finished_ipcs.push_back(ev);
    }
  }
  // If this is the last fd pointing to the target file and the file
  // is a socket, finish the socket and collect the Socket event.
  else if (lp->fds[fd].get_target_file_references() == 1
      && lp->fds[fd].get_target_file_type() == OpenFile::OS_FILE_TYPE_SOCKET) {
    // TODO deal with inherited socket file descriptors correctly
    // (should we finish the socket already if the parent closes it?)
    Socket *sock = (Socket*) lp->fds[fd].get_target_file();
    sock->close(se->event_time);
    SocketEvent *ev = sock->to_socket_event();
    // only add complete sockets to the list of IPC events
    if (ev) {
      finished_sockets.push_back(ev);
    }
  }

  std::string fd_str = lp->fds[fd].str();
  lp->fds.erase(fd);
  LOGGER_LOG_DEBUG("[" << lp->pid << "] Closing file descriptor " << fd_str << " in "
      << lp->pid << ". Process now has " << lp->fds.size()
      << " open file descriptors.");
}

void ProcessTable::dup2(SyscallEvent *se) {
  // convert base-16 fds to base-10
  int old_fd = hex_to_dec(se->arg0);
  int new_fd = hex_to_dec(se->arg1);
  LOGGER_LOG_DEBUG("dup2 called with " << old_fd << " and " << new_fd);

  LiveProcess *lp = get_live_process(se->pid);
  if (lp->fds.find(old_fd) != lp->fds.end()) {
    // we have seen the dup'ed file descriptor opened, now check
    // if it's a read/write pipe and if it's duped to stdin/stdout
    FileDescriptor fd = lp->fds[old_fd];
    if (fd.get_type() == osm_fd_pipe_read && new_fd == 0) {
      // the pipe read end has been set up
      ((Pipe*) fd.get_target_file())->set_reader_process(lp->pid, lp->start_time_utc);
      LOGGER_LOG_DEBUG("Setting pipe reader " << lp->pid << " - " << lp->start_time_utc);
    } else if (fd.get_type() == osm_fd_pipe_write && new_fd == 1) {
      // the pipe write end has been set up
      ((Pipe*) fd.get_target_file())->set_writer_process(lp->pid, lp->start_time_utc);
      LOGGER_LOG_DEBUG("Setting pipe writer " << lp->pid << " - " << lp->start_time_utc);
    }
  }
}

void ProcessTable::socket(SyscallEvent *se) {
  // convert base-16 socket fd to base-10
  int fd = hex_to_dec(std::to_string(se->rc));

  // create the Socket and add to process
  LiveProcess *lp = get_live_process(se->pid);
  std::shared_ptr<Socket> sock = std::make_shared<Socket>();
  sock->open(lp->pid, se->event_time);
  FileDescriptor sock_fd(osm_fd_socket, fd, sock);
  lp->fds[fd] = sock_fd;

  LOGGER_LOG_DEBUG("[" << lp->pid << "] Added file descriptor " << lp->fds[fd].str() << " to lp "
      << lp->pid << ". Process now has " << lp->fds.size() << " open file descriptors.");
}

void ProcessTable::connect(SyscallEvent *se) {
  // convert base-16 fds to base-10
  int sockfd = hex_to_dec(se->arg0);

  LiveProcess *lp = get_live_process(se->pid);
  if (lp->fds.find(sockfd) == lp->fds.end()) {
    // we haven't seen the open of that socket, which means it's either
    // prehistoric or we're not tracking that socket's domain (only
    // AF_INET tested so far)
    LOGGER_LOG_DEBUG("Didn't see open for socket " << se->arg0 << " " << se->arg1);
    return;
  }

  // see SyscallEvent::data for the interpretation on connect
  std::string remote_addr = se->data[0];
  uint16_t remote_port = std::stoi(se->data[1]);

  // connect the socket
  FileDescriptor fd = lp->fds[sockfd];
  Socket *sock = (Socket*) fd.get_target_file();
  sock->connect(remote_addr, remote_port, se->event_time);

  // record the connection event
  SocketConnectEvent *ev = sock->to_socket_connect_event();
  if (ev) {
    finished_socket_connects.push_back(ev);
  }
  LOGGER_LOG_DEBUG("[" << lp->pid << "] connected to " << remote_addr << ":" << remote_port);
}

void ProcessTable::bind(SyscallEvent *se) {
  // convert base-16 fds to base-10
  int sockfd = hex_to_dec(se->arg0);

  LiveProcess *lp = get_live_process(se->pid);
  if (lp->fds.find(sockfd) == lp->fds.end()) {
    // we haven't seen the open of that socket, which means it's either
    // prehistoric or we're not tracking that socket's domain (only
    // AF_INET tested so far)
    LOGGER_LOG_DEBUG("Didn't see open for socket " << se->arg0 << " " << se->arg1);
    return;
  }

  // see SyscallEvent::data for the interpretation on bind
  std::string localAddr = se->data[0];
  uint16_t localPort = std::stoi(se->data[1]);

  // bind the socket
  FileDescriptor fd = lp->fds[sockfd];
  // TODO how to deal with the case in which a host has more than one hostname?
  ((Socket*) fd.get_target_file())->bind(localPort);

  LOGGER_LOG_DEBUG("[" << lp->pid << "] bound to " << localAddr << ":" << localPort);
}

void ProcessTable::execve(SyscallEvent *se) {
  // see SyscallEvent::data for the interpretation on execve
  std::string cwd = se->data[0];
  std::vector<std::string> exec_cmd_line { se->data.begin() + 1, se->data.end() };
  LiveProcess *lp = get_live_process(se->pid);
  lp->execve(cwd, exec_cmd_line);
}

void ProcessTable::setpgid(SyscallEvent *se) {
  // get relevant info from the event (ids are hex-encoded)
  osm_pid_t affected_pid = strtol(se->arg0.c_str(), NULL, 16);
  osm_pgid_t new_pgid = strtol(se->arg1.c_str(), NULL, 16);

  // handle special values
  if (affected_pid == 0) {
    affected_pid = se->pid;
  }
  if (new_pgid == 0) {
    new_pgid = affected_pid;
  }

  // heuristic to identify process group leaders (only setpgid assignments for the
  // process group leader will cause a new process group to be created)
  bool is_pgroup_leader = false;
  if (new_pgid == affected_pid) {
    is_pgroup_leader = true;
  }

  LOGGER_LOG_DEBUG("ProcessTable::setpgid: Process " << affected_pid << " is assigned to pgroup "
      << new_pgid << " isPgroupLeader "<< is_pgroup_leader);

  // get the affected process (might be prehistoric)
  // since the affected process may not be the caller, it may not exist yet
  LiveProcess *affected_process = add_process_if_unseen(affected_pid);
  osm_pgid_t old_pgid = affected_process->pgid;
  affected_process->setpgid(new_pgid);

  LiveProcessGroup *old_lpg = get_live_process_group(old_pgid);
  LiveProcessGroup *new_lpg = get_live_process_group(new_pgid);
  LOGGER_LOG_DEBUG("ProcessTable::setpgid: oldPgid " << old_pgid << " old_lpg "
      << old_lpg << " newPgid " << new_pgid << "  new_lpg " << new_lpg
      << " isPgroupLeader " << is_pgroup_leader);

  // old_pgid could be equal to new_pgid so we save the call to
  // remove_process_group_from_state until the end
  if (old_lpg) {
    old_lpg->remove_process(affected_process->pid);
  }
  if (new_lpg) {
    new_lpg = add_process_to_process_group(affected_process, new_lpg, se->event_time);
  } else {
    // No such lpg, is group new or prehistoric? We assume new groups are only formed
    // when the process is obviously the pgroup leader, which seems to be the general convention.
    if (is_pgroup_leader) {
      // ceate pgroup
      new_lpg = new LiveProcessGroup(new_pgid, se->event_time);
      LOGGER_LOG_DEBUG("ProcessTable::setpgid: process group " << new_pgid << ":" << new_lpg);
      register_live_process_group(new_lpg);
      add_process_to_process_group(affected_process, new_lpg, se->event_time);
    } else {
      // process is joining a prehistoric group, do nothing
      LOGGER_LOG_DEBUG("ProcessTable::setpgid: Process " << affected_process->pid
          << " is joining prehistoric pgroup " << affected_process->pgid);
    }
  }

  if (old_lpg && old_lpg->is_empty()) {
    finalize_process_group(old_lpg, se->event_time);
  }

  return;
}

void ProcessTable::exit(SyscallEvent *se) {
  LiveProcess *lp = get_live_process(se->pid);

  if (lp->is_thread) {
    LOGGER_LOG_DEBUG("Thread " << std::to_string(lp->pid) << " called exit()");
    finalize_thread((LiveThread*) lp, se->event_time, true);
  } else {
    LOGGER_LOG_DEBUG("Process " << std::to_string(lp->pid) << " called exit()");
    finalize_process(lp, se->event_time);
  }
}

void ProcessTable::exit_group(SyscallEvent *se) {
  LiveProcess *lp = get_live_process(se->pid);
  assert(lp);

  if (lp->is_thread) {
    // if a thread has called exit_group, we need to finalize its parent
    LiveProcess *parent = ((LiveThread*) lp)->parent;
    LOGGER_LOG_DEBUG("Thread " << std::to_string(lp->pid) << " called exit_group()");
    finalize_process(parent, se->event_time);
  } else {
    LOGGER_LOG_DEBUG("Process " << std::to_string(lp->pid) << " called exit_group()");
    finalize_process(lp, se->event_time);
  }
}

void ProcessTable::finalize_process(LiveProcess *lp, const std::string &death_time) {
  lp->exit_group(death_time);

  // kill all associated threads
  for (std::map<osm_pid_t, LiveThread*>::iterator it = lp->threads.begin();
      it != lp->threads.end(); it++) {
    LOGGER_LOG_DEBUG("Killing thread " << std::to_string(it->second->pid));
    finalize_thread(it->second, death_time, false);
  }
  lp->threads.clear();

  // finish the lpg (might not exist, could be prehistoric)
  LiveProcessGroup *lpg = get_live_process_group(lp->pgid);
  if (lpg) {
    // Update process group membership.
    lpg->remove_process(lp->pid);
    if (lpg->is_empty()) {
      finalize_process_group(lpg, death_time);
      lpg = nullptr;
    }
  }

  remove_process_from_state(lp);
  return;
}

void ProcessTable::finalize_thread(LiveThread *lt, const std::string &death_time,
    bool delete_from_parent) {
  lt->exit_group(death_time);
  remove_thread_from_state(lt, delete_from_parent);
  return;
}

void ProcessTable::finalize_process_group(LiveProcessGroup *lpg, const std::string &death_time) {
  lpg->make_dead(death_time);
  remove_process_group_from_state(lpg, death_time);
  return;
}

void ProcessTable::remove_process_from_state(LiveProcess *lp) {
  if (lp) {
    LOGGER_LOG_DEBUG("Deleting lp " << lp->pid << "(" << lp << ")");
    assert(get_live_process(lp->pid) == lp);
    live_processes.erase(lp->pid);

    ProcessEvent *evt = lp->to_process_event();
    dead_processes.push_back(evt);
    delete lp;
  }
}

void ProcessTable::remove_thread_from_state(LiveThread *lt, bool delete_from_parent) {
  if (lt) {
    LOGGER_LOG_DEBUG("Deleting lt " << lt->pid << "(" << lt << ")");
    assert(get_live_process(lt->pid) == lt);

    // delete thread from parent
    if (delete_from_parent) {
      lt->parent->threads.erase(lt->pid);
    }

    // Delete thread from process table. So far, we don't add threads to the list
    // of dead processes and do not collect them as process events as we don't have
    // a use for them in the database.
    live_processes.erase(lt->pid);
    delete lt;
  }
}

void ProcessTable::remove_process_group_from_state(LiveProcessGroup *lpg,
    const std::string &time) {
  if (lpg) {
    LOGGER_LOG_DEBUG("Deleting lpg " << lpg->pgid << "(" << lpg << ")");
    assert(get_live_process_group(lpg->pgid) == lpg);
    live_process_groups.erase(lpg->pgid);

    ProcessGroupEvent *evt = lpg->to_process_group_event();
    dead_process_groups.push_back(evt);
    delete lpg;
  }
}

LiveProcessGroup* ProcessTable::try_to_add_process_to_process_group(const LiveProcess *lp,
    std::string &join_time) {
  LiveProcessGroup *lpg = get_live_process_group(lp->pgid);
  if (lpg) {
    lpg = add_process_to_process_group(lp, lpg, join_time);
  }
  return lpg;
}

LiveProcessGroup* ProcessTable::add_process_to_process_group(const LiveProcess *lp,
    LiveProcessGroup *lpg, std::string &joinTime) {
  assert(lp);
  assert(lpg);
  assert(get_live_process_group(lpg->pgid));
  LOGGER_LOG_DEBUG("ProcessTable::addProcessToProcessGroup: Adding lp " << lp->pid
      << "(" << lp << ") lpg " << lp->pgid << " to liveProcessGroup " << lpg);

  // Check if already present. This can only be the case if
  // the old pgid died, and the pid and pgid have been re-used.
  // If so, mark it dead and start a new one.
  if (lpg->has_process(lp->pid)) {
    // TODO lp should track that it replaced a zombie, and we should assert that here
    // (otherwise there is a nasty bug)
    assert(!"I was hoping this would never happen");
    LOGGER_LOG_ERROR("ProcessTable::addProcessToProcessGroup: " << lp->pgid
        << " was a zombie process group, retiring it.");

    // retire the lpg
    finalize_process_group(lpg, joinTime);
    // replace the lpg
    lpg = new LiveProcessGroup(lp->pgid, joinTime);
    LOGGER_LOG_DEBUG("ProcessTable::addProcessToProcessGroup: process group "
        << lp->pgid << ": " << lpg);
    register_live_process_group(lpg);
  }

  lpg->add_process(lp->pid);
  return lpg;
}

/*------------------------------
 * LiveProcess
 *------------------------------*/

LiveProcess::LiveProcess(const LiveProcess *parent, osm_pid_t pid,
    std::string start_time_utc, bool inherit_fds) :
    pid { pid },
    start_time_utc { start_time_utc },
    is_thread { false } {
  ppid = parent->pid;
  pgid = parent->pgid;

  if (inherit_fds) {
    for (std::pair<int, FileDescriptor> fd : parent->fds) {
      fds[fd.first] = fd.second;
    }
  }

  exec_cwd = parent->exec_cwd;
  exec_cmd_line = parent->exec_cmd_line;
}

LiveProcess::LiveProcess(const SyscallEvent *se) :
    pid { -1 },
    ppid { -1 },
    pgid { -1 },
    exec_cwd { "UNKNOWN" },
    exec_cmd_line { "UNKNOWN" },
    start_time_utc { EPOCH_TIME_UTC },
    finish_time_utc { FUTURE_TIME_UTC },
    is_thread { false } {
  pid = se->pid;
  ppid = se->ppid;

  // Don't set start_time_utc_ to se->eventTime_. This might interfere with queries on
  // processes alive at a certain time based on interactions with other event streams
  // (like Scale access events).
}

LiveProcess::LiveProcess(osm_pid_t pid) :
    pid { pid }, ppid { -1 },
    pgid { -1 },
    exec_cwd { "UNKNOWN" },
    exec_cmd_line { "UNKNOWN" },
    start_time_utc { EPOCH_TIME_UTC },
    finish_time_utc { FUTURE_TIME_UTC },
    is_thread { false } {
}

void LiveProcess::setpgid(osm_pgid_t pgid) {
  if (pgid == 0) {
    this->pgid = this->pid;
  } else {
    this->pgid = pgid;
  }
}

void LiveProcess::execve(std::string cwd, std::vector<std::string> cmd_line) {
  exec_cwd = cwd;
  exec_cmd_line = cmd_line;
}

void LiveProcess::vfork(std::string start_time, osm_pid_t p_pid, osm_pgid_t p_gid) {
  start_time_utc = start_time;
  ppid = p_pid;
  pgid = p_gid;
}

void LiveProcess::exit_group(std::string finish_time) {
  finish_time_utc = finish_time;
}

ProcessEvent* LiveProcess::to_process_event() {
  return new ProcessEvent(pid, ppid, pgid, exec_cwd, exec_cmd_line,
      start_time_utc, finish_time_utc);
}

/*------------------------------
 * LiveThread
 *------------------------------*/

LiveThread::LiveThread(LiveProcess *parent, osm_pid_t pid, std::string start_time_utc) :
    LiveProcess(parent, pid, start_time_utc, false),
    parent { parent } {
  is_thread = true;
}

/*------------------------------
 * LiveProcessGroup
 *------------------------------*/

LiveProcessGroup::LiveProcessGroup(osm_pgid_t pgid, const std::string &start_time_utc) :
    pgid { pgid },
    start_time_utc { start_time_utc },
    finish_time_utc { "" } {
}

osm_rc_t LiveProcessGroup::add_process(osm_pid_t process) {
  current_members.insert(process);
  return osm_rc_ok;
}

bool LiveProcessGroup::has_process(osm_pid_t process) const {
  return (current_members.find(process) != current_members.end());
}

osm_rc_t LiveProcessGroup::remove_process(osm_pid_t process) {
  assert(has_process(process));
  current_members.erase(process);
  former_members.insert(process);
  return osm_rc_ok;
}

bool LiveProcessGroup::is_empty() const {
  return current_members.empty();
}

void LiveProcessGroup::make_dead(const std::string &time) {
  if (!is_empty()) {
    LOGGER_LOG_DEBUG("LiveProcessGroup::makeDead: process group %d is non-empty " << pgid);
  }

  // must only be called once
  assert(!finish_time_utc.length());

  former_members = current_members;
  current_members.clear();
  finish_time_utc = time;
}

ProcessGroupEvent* LiveProcessGroup::to_process_group_event() {
  return new ProcessGroupEvent(pgid, start_time_utc, finish_time_utc);
}
