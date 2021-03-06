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

#ifndef EVENT_SCALE_EVENT_H_
#define EVENT_SCALE_EVENT_H_

#include <map>

#include "event.h"

class FSEvent: public Event {
protected:
  osm_pid_t pid;
  int inode;
  long bytes_read;
  long bytes_written;

  std::string event;
  std::string event_time;
  std::string cluster_name;
  std::string fs_name;
  std::string path;
  std::string dst_path;
  std::string mode;
  std::string version_hash;

  FSEvent() {}

public:
  FSEvent(const std::string &serialized_event);
  FSEvent(osm_pid_t pid, int inode, long bytes_read, long bytes_written,
      std::string event, std::string event_time, std::string cluster_name,
      std::string fs_name, std::string path, std::string dst_path,
      std::string mode, std::string version_hash);
  ~FSEvent() {}

  virtual std::string serialize() const override;
  virtual std::string format_for_dst(ConsumerDestination c_dst) const override;
  virtual std::string get_value(std::string field) const override;
  virtual EventType get_type() const override {
    return FS_EVENT;
  }

  void set_version_hash(std::string hash) { version_hash = hash; }
};

class FSEventJson: public FSEvent {
protected:
  static std::map<std::string, std::string> WFEVENT_TO_FSEVENT;
  static std::map<long, std::string> COOKIE_STATE;

public:
  FSEventJson(const std::string &serialized_event);
  FSEventJson(osm_pid_t pid, int inode, long bytes_read, long bytes_written,
      std::string event, std::string event_time, std::string cluster_name,
      std::string fs_name, std::string path, std::string dst_path,
      std::string mode, std::string version_hash) : FSEvent(pid, inode, bytes_read,
          bytes_written, event, event_time, cluster_name, fs_name, path, dst_path,
          mode, version_hash) {}
  ~FSEventJson() {}
};

#endif /* EVENT_SCALE_EVENT_H_ */
