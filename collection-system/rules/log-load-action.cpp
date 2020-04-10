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

#include <regex>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "action.h"
#include "db-output-stream.h"

/**
 * Helper function to extract a record from a line based on a specified
 * delimiter and a set of LogLoadFields.
 */
std::string extract_record_from_line(std::string line, std::string delimiter,
    std::vector<LogLoadField*> fields, std::shared_ptr<IntermediateMessage> msg) {
  size_t pos = 0;
  std::stringstream record;
  std::string token;
  std::vector<std::string> tokens;

  while ((pos = line.find(delimiter)) != std::string::npos) {
    token = line.substr(0, pos);
    tokens.push_back(token);
    line.erase(0, pos + delimiter.length());
  }
  tokens.push_back(line);

  // TODO add range check on fieldIDs and tokens
  for (unsigned int k = 0; k < fields.size(); k++) {
    LogLoadField *field = fields.at(k);
    if (field->is_range_field()) {
      std::string field_value;
      int parse_until = field->get_until_field_id() == -1 ?
              tokens.size() - 1 : field->get_until_field_id();

      for (int j = field->get_field_id(); j <= parse_until; j++) {
        field_value += tokens.at(j) + ((j == parse_until) ? "" : " ");
      }

      if (field->is_timestamp_field()) {
        record << convert_date_field(field_value, field);
      } else {
        record << field_value;
      }
      record << ((k == fields.size() - 1) ? "" : ",");
    } else if (field->is_event_field()) {
      std::string event_field_val = msg->get_value(field->get_field_name());
      record << event_field_val << ((k == fields.size() - 1) ? "" : ",");
    } else if (field->is_composite_field()) {
      std::vector<int> ids = field->get_field_ids();
      for (unsigned int j = 0; j < ids.size(); j++) {
        record << tokens.at(ids.at(j));
      }
      record << ",";
    } else {
      if (field->is_timestamp_field()) {
        record << convert_date_field(tokens.at(field->get_field_id()), field)
            << ((k == fields.size() - 1) ? "" : ",");
      } else {
        record << tokens.at(field->get_field_id())
            << ((k == fields.size() - 1) ? "" : ",");
      }
    }
  }
  record << std::endl;

  return record.str();
}

LogLoadAction::LogLoadAction(std::string action) {
  // check that the action has the right syntax
  if (!std::regex_match(action, LOG_LOAD_SYNTAX)) {
    LOG_ERROR("LogLoadAction " << action << " is not specified correctly.");
    throw std::invalid_argument(action + " not specified correctly.");
  }

  // parse action
  size_t match_pos = action.find("MATCH", 0);
  event_field = action.substr(LOG_LOAD_RULE.length() + 1,
      match_pos - (LOG_LOAD_RULE.length() + 2));

  size_t fields_pos = action.find("FIELDS", match_pos);
  matching_string = std::regex("(.*)" + action.substr(
      match_pos + 6, fields_pos - (match_pos + 5 + 2)) + "(.*)");

  size_t delim_pos = action.find("DELIM", fields_pos);
  std::string fields_string = action.substr(fields_pos + 7, delim_pos - (fields_pos + 6 + 2));
  std::stringstream ss(fields_string);
  std::string field;
  while (getline(ss, field, ',')) {
    try {
      fields.push_back(new LogLoadField(field));
    } catch (std::invalid_argument &e) {
      LOG_ERROR("Problems while parsing LogLoad field " << field
          << ". Reason invalid argument to " << e.what()
          << ". Field will not be added to the LogLoad fields.");
    }
  }

  size_t into_pos = action.find("INTO", delim_pos);
  delimiter = action.substr(delim_pos + 6, into_pos - (delim_pos + 5 + 2));

  size_t using_pos = action.find("USING", into_pos);
  std::string connection_string = action.substr(into_pos + 4 + 1,
      using_pos - (into_pos + 4 + 2));
  size_t at_pos = connection_string.find("@", 0);
  std::string user_password = connection_string.substr(0, at_pos);
  size_t colon_pos = user_password.find(":");
  target_db.username = user_password.substr(0, colon_pos);
  target_db.password = user_password.substr(colon_pos + 1, user_password.length());

  std::string server_table = connection_string.substr(at_pos + 1, connection_string.length());
  size_t slash_pos = server_table.find("/");
  target_db.hostname = server_table.substr(0, slash_pos);
  target_db.tablename = server_table.substr(slash_pos + 1, server_table.length());
  target_db.db_schema = action.substr(using_pos + 6, action.length() - (using_pos + 6));

  // set up ODBC connections and output stream
  target_db_wrapper = new OdbcWrapper(DEFAULT_DSN, target_db.username, target_db.password);
  if (target_db_wrapper->connect() != ODBC_SUCCESS) {
    LOG_ERROR("Error while connecting to target DB " << DEFAULT_DSN);
    throw DBConnectionException();
  }

  out = new DBOutputStream(DEFAULT_DSN, target_db.username, target_db.password,
      target_db.db_schema, target_db.tablename, false);
  out->open();
}

LogLoadAction::~LogLoadAction() {
  for (unsigned int i = 0; i < fields.size(); i++) {
    delete fields.at(i);
  }
  // destructor handles disconnection
  delete target_db_wrapper;
}

int LogLoadAction::execute(std::shared_ptr<IntermediateMessage> msg) {
  LOG_DEBUG("Executing LogLoadAction " << this->str());
  int rc = NO_ERROR;

  // get inode of file to deal with log rollover
  std::string path = msg->get_value(event_field);
  struct stat sb;
  if (stat(path.c_str(), &sb) < 0) {
    LOG_ERROR("stat() failed with " << strerror(errno) << "."
        << " Can't retrieve inode for " << path << ". "
        << "Exiting LogLoadAction " << this->str()
        << " for received message " << msg->normalize(CD_DB2));
    return ERROR_NO_RETRY;
  }
  unsigned long long inode = sb.st_ino;

  // retrieve existing state
  std::pair<long long int, unsigned long long> state;
  if (parsing_state.find(path) == parsing_state.end()) {
    // check if we have state stored in the database
    char stateBuffer[1024];
    rc = lookup_state(target_db_wrapper, stateBuffer, rule_id, path);
    if (rc == ERROR_NO_RETRY) {
      LOG_ERROR("Problems while trying to restore state for " << this->str()
          << ". Will start" << " parsing " << path << " from 0.");
      state.first = 0;
      state.second = inode;
      parsing_state[path] = state;
      // try to add state to DB
      insert_state(target_db_wrapper, rule_id, std::to_string(parsing_state[path].first) + ","
          + std::to_string(parsing_state[path].second), path);
    } else {
      std::string existing_state = (rc == NO_ERROR) ? std::string(stateBuffer) : "";
      if (!existing_state.empty()) {
        // we had state for this value in the database
        // the format of the retrieved state is 'offset,inode'
        size_t pos = existing_state.find(",");
        state.first = std::stoll(existing_state.substr(0, pos));
        state.second = std::stoll(existing_state.substr(pos + 1, std::string::npos));
        parsing_state[path] = state;
        LOG_INFO("LogLoadAction " << this->str() << ": restored map state");
      } else {
        // we haven't seen this value before so initialize it in the map and the database
        state.first = 0;
        state.second = inode;
        parsing_state[path] = state;
        rc = insert_state(target_db_wrapper, rule_id, std::to_string(parsing_state[path].first)
            + "," + std::to_string(parsing_state[path].second), path);
        if (rc != NO_ERROR) {
          LOG_ERROR("Problems while adding state for new rule " << this->str()
              << ". State can't be backed up at the moment.");
        } else {
          LOG_INFO("LogLoadAction " << this->str() << ": no existing state found");
        }
      }
    }
  }

  // check if inode has changed
  if (parsing_state[path].second != inode) {
    // if inode has changed due to log rollover (i.e. same path but different inode)
    // reset the parsing state to store the new inode and start parsing from 0 again
    parsing_state[path].first = 0;
    parsing_state[path].second = inode;
    // TODO could there still be unread data in the old file?
  }

  // open log file and seek to last parsed position
  std::ifstream infile(path);
  infile.seekg(parsing_state[path].first);

  // We are reading the file in batches of 4KB and extracting data line by
  // line. If we're at the end of the file and we have not ended the current
  // line, we store the partial line in lineFragment. On the next action trigger,
  // we read until the end of the previously stored partial line and combine it
  // with the lineFragment to restore the full line.
  //
  // NB: This code is not yet able to combine lines *across* read calls and hence,
  // we assume that no line exceeds 4KB, i.e. we can always read at least one line break
  // per read call.
  char buffer[MAX_LINE_LENGTH];
  std::vector<std::string> records;
  std::string line;
  bool found_entries = false;
  bool read = true;

  while (read) {
    infile.read(buffer, MAX_LINE_LENGTH);
    size_t bytes_read = infile.gcount();

    // error handling
    if (infile.eof()) {
      read = false;
    }
    if (infile.bad()) {
      LOG_ERROR("Error while reading, bad stream. LogLoad not reading from " << path);
      read = false;
    }

    int lineoffset = 0;
    for (uint i = 0; i < bytes_read; i++) {
      if (buffer[i] == '\n') {
        // read one line
        int line_length = i - lineoffset;
        if (line_overflow > 0) {
          line_length += line_overflow;
        }

        std::vector<char> line(line_length);
        if (line_overflow > 0) {
          LOG_DEBUG("Appending previous line fragment to currently read line.");
          for (unsigned j = 0; j < line_fragment.size(); j++) {
            line[j] = line_fragment[j];
          }
          for (unsigned j = lineoffset; j < i; j++) {
            line[j - lineoffset + line_overflow] = buffer[j];
          }
          line_overflow = 0;
        } else {
          // insert inserts from [lineoffset, i), i.e. buffer[i] is *excluded*
          for (unsigned j = lineoffset; j < i; j++) {
            line[j - lineoffset] = buffer[j];
          }
        }

        lineoffset = i + 1;
        std::string line_str(line.begin(), line.end());
        LOG_DEBUG("Read line '" << line_str << "'");

        // match the line to find possible records to extract
        if (std::regex_match(line_str, matching_string)) {
          std::string record = extract_record_from_line(line_str, delimiter, fields, msg);
          records.push_back(record);
          found_entries = true;
        }
      }
      if (i == bytes_read - 1 && buffer[i] != '\n') {
        // we're not ending with a new line
        // store everything from the last line break in a buffer
        line_fragment.resize(bytes_read - lineoffset);
        for (unsigned j = lineoffset; j < bytes_read; j++) {
          line_fragment[j - lineoffset] = buffer[j];
        }
        line_overflow = bytes_read - lineoffset;
        std::string line_fragment_str(line_fragment.begin(), line_fragment.end());
        LOG_DEBUG("Read data doesn't end with new line, storing broken line '"
            << line_fragment_str << "' with line overflow " << line_overflow);
      }
    }

    parsing_state[path].first = parsing_state[path].first + bytes_read;
  }
  // TODO this needs to be synchronized if we make action handling multi-threaded
  rc = update_state(target_db_wrapper, rule_id, std::to_string(parsing_state[path].first) + ","
      + std::to_string(parsing_state[path].second), path);
  if (rc != NO_ERROR) {
    LOG_ERROR("Problems while updating state for rule " << this->str()
        << ". State can't be backed up at the moment.");
  }
  infile.close();

  if (found_entries) {
    rc = out->send_batch(records);
    if (rc != NO_ERROR) {
      LOG_ERROR("Problems while bulk loading data into DB."
          << " Provenance may be incomplete. Action: " << this->str());
    }
  }

  return rc;
}

std::string LogLoadAction::str() const {
  // convert the fields vector to a string
  std::stringstream ss;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      ss << ",";
    }
    ss << fields[i];
  }

  return "LOGLOAD " + event_field + " MATCH " + " FIELDS " + ss.str() + " DELIM "
      + delimiter + " INTO " + target_db.username + "@" + target_db.hostname
      + "/" + target_db.tablename + " USING " + target_db.db_schema;
}

std::string LogLoadAction::get_type() const {
  return LOG_LOAD_RULE;
}