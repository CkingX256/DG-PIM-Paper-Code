#pragma once
namespace Ramulator {
struct INumericSystem {
  virtual bool numeric_finished() const = 0;
  virtual ~INumericSystem() = default;
};
}
