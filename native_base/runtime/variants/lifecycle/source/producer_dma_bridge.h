#pragma once
#include "base/request.h"
namespace Ramulator {

struct IProducerWriteIssue {
  virtual void producer_write_issued(const Request&, Clk_t) = 0;
  virtual ~IProducerWriteIssue() = default;
};
}
