#pragma once

#include "TracePayloadBuffer.h"
#include "td/actor/actor.h"

#include <memory>

namespace mch {

struct ClassifyJob {
  ClassifyInput input;
  ParsedBlockPtr block;
};

class ClassifyWorker : public td::actor::Actor {
 public:
  ClassifyWorker(std::shared_ptr<const MchEnginePrep> prep, ClassifyPolicy policy);
  void classify(ClassifyJob job, td::Promise<TraceClassificationResult> promise);

 private:
  std::shared_ptr<const MchEnginePrep> prep_;
  ClassifyPolicy policy_;
};

}  // namespace mch
