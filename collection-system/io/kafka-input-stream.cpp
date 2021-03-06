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

#include "kafka-input-stream.h"
#include "logger.h"
#include "error.h"
#include "config.h"

KafkaInputStream::KafkaInputStream(std::string t, std::string b, std::string g) :
    topic(t), brokers(b), group_id(g) {
  conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
  consumer = nullptr;
}

KafkaInputStream::~KafkaInputStream() {
  if (conf) delete conf;
  if (consumer) delete consumer;
}

int KafkaInputStream::open() {
  std::string errstr;

  // set brokers
  if (conf->set("metadata.broker.list", brokers, errstr) != RdKafka::Conf::CONF_OK) {
    LOGGER_LOG_ERROR("Couldn't set brokers for KafkaInputStream: " << errstr);
    return ERROR_NO_RETRY;
  }
  // set group ID
  if (conf->set("group.id", group_id, errstr) != RdKafka::Conf::CONF_OK) {
    LOGGER_LOG_ERROR("Couldn't set group ID for KafkaInputStream: " << errstr);
    return ERROR_NO_RETRY;
  }
  // enable auto commit
  if (conf->set("enable.auto.commit", "true", errstr) != RdKafka::Conf::CONF_OK) {
    LOGGER_LOG_ERROR("Couldn't enable auto commit for KafkaInputStream: " << errstr);
    return ERROR_NO_RETRY;
  }
  // set 60s session timeout
  if (conf->set("session.timeout.ms", "60000", errstr) != RdKafka::Conf::CONF_OK) {
    LOGGER_LOG_ERROR("Couldn't set session timout for KafkaInputStream: " << errstr);
    return ERROR_NO_RETRY;
  }
  // set SASL authentication options
  if (!Config::config[Config::CKEY_KAFKA_SASL_USER].empty() &&
      !Config::config[Config::CKEY_KAFKA_SASL_PASS].empty()) {
    if (conf->set("security.protocol", "sasl_plaintext", errstr) != RdKafka::Conf::CONF_OK) {
      LOGGER_LOG_ERROR("Failed to set security protocol for KafkaInputStream: " << errstr);
      return ERROR_NO_RETRY;
    }
    if (conf->set("sasl.mechanisms", "SCRAM-SHA-512", errstr) != RdKafka::Conf::CONF_OK) {
      LOGGER_LOG_ERROR("Failed to set sasl mechanism for KafkaInputStream: " << errstr);
      return ERROR_NO_RETRY;
    }
    if (conf->set("sasl.username", Config::config[Config::CKEY_KAFKA_SASL_USER],
        errstr) != RdKafka::Conf::CONF_OK) {
      LOGGER_LOG_ERROR("Failed to set sasl username for KafkaInputStream: " << errstr);
      return ERROR_NO_RETRY;
    }
    if (conf->set("sasl.password", Config::config[Config::CKEY_KAFKA_SASL_PASS],
        errstr) != RdKafka::Conf::CONF_OK) {
      LOGGER_LOG_ERROR("Failed to set sasl password for KafkaInputStream: " << errstr);
      return ERROR_NO_RETRY;
    }
  }

  // create consumer
  consumer = RdKafka::KafkaConsumer::create(conf, errstr);
  if (!consumer) {
    LOGGER_LOG_ERROR("Couldn't create Kafka consumer: " << errstr);
    return ERROR_NO_RETRY;
  }

  // subscribe to topic
  RdKafka::ErrorCode err = consumer->subscribe({ topic });
  if (err) {
    LOGGER_LOG_ERROR("Couldn't subscribe to topic " << topic << ": " << RdKafka::err2str(err));
    return ERROR_NO_RETRY;
  }

  return NO_ERROR;
}

void KafkaInputStream::close() {
  consumer->close();
}

int KafkaInputStream::recv(std::string &next_msg) {
  int rc;

  // get next message from Kafka
  RdKafka::Message *msg = consumer->consume(TIMEOUT_MS);
  switch (msg->err()) {
  case RdKafka::ERR__TIMED_OUT:
    rc = ERROR_RETRY;
    break;
  case RdKafka::ERR_NO_ERROR:
    if (static_cast<int>(msg->len()) > 0) {
      char msg_buffer[static_cast<int>(msg->len())];
      sprintf(msg_buffer, "%.*s\n", static_cast<int>(msg->len()),
          static_cast<const char*>(msg->payload()));
      next_msg = std::string(msg_buffer);
      // TODO why are those messages not delivered as part of RD_KAFKA_RESP_ERR__PARTITION_EOF errors?
      if (next_msg == "Broker: No more messages\n") {
        rc = ERROR_RETRY;
      } else {
        rc = NO_ERROR;
      }
    } else {
      rc = ERROR_RETRY;
      LOGGER_LOG_WARN("Received empty message.");
    }
    break;
  case RdKafka::ERR__UNKNOWN_TOPIC:
  case RdKafka::ERR__UNKNOWN_PARTITION:
    LOGGER_LOG_ERROR("Consume failed with: " << msg->errstr());
    rc = ERROR_NO_RETRY;
    break;
  default:
    // TODO Check whether ERROR_RETRY is ok for all other error codes (see rdkafkacpp.h).
    LOGGER_LOG_DEBUG("Consume returned error: " << msg->errstr());
    rc = ERROR_RETRY;
    break;
  }

  delete msg;
  return rc;
}
