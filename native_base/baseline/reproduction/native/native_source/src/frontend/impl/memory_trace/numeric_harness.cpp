#include "frontend/frontend.h"
#include "memory_system/numeric_bridge.h"
namespace Ramulator {

class NumericHarness : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, NumericHarness, "NumericHarness", "Native numerical attention harness")
  INumericSystem* system = nullptr;
public:
  void init() override {m_clock_ratio=param<uint>("clock_ratio").required();}
  void setup(IFrontEnd*, IMemorySystem* memory) override {
    system=dynamic_cast<INumericSystem*>(memory);
    if(!system) throw std::runtime_error("NumericHarness requires NumericPIMDRAM");
  }
  void tick() override {}
  bool is_finished() override {return system && system->numeric_finished();}
};
}
