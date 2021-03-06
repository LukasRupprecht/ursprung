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

#ifndef PROV_GPFS_CONSUMER_H
#define PROV_GPFS_CONSUMER_H

#include "abstract-consumer.h"
#include "rule-engine.h"

class ScaleConsumer: public AbstractConsumer {
private:
  bool track_versions = false;
  virtual int receive_event(ConsumerSource csrc, evt_t event) override;

public:
  ScaleConsumer(ConsumerSource csrc, std::unique_ptr<MsgInputStream> in,
      ConsumerDestination cdst, std::unique_ptr<MsgOutputStream> out,
      bool track_versions, uint32_t batchsize = 10000) :
      AbstractConsumer(csrc, std::move(in), cdst, std::move(out), batchsize),
      track_versions { track_versions } {}
  ~ScaleConsumer() {};
};

#endif
