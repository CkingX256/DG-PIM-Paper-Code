#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include "frontend/frontend.h"
#include "memory_system/memory_system.h"
#include "base/exception.h"

namespace Ramulator {


class PIMPhaseTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, PIMPhaseTrace, "PIMPhaseTrace", "Native PIM traffic with explicit drain fences.")
  struct Op {int type; Addr_t addr;};
  std::vector<Op> ops;
  size_t pos=0, sent=0, fences=0;
  uint64_t cycles=0, stalls=0, drain_until=0;
  uint64_t drain=64, limit=10000000;
  bool draining=false;
public:
  void init() override {
    m_clock_ratio=param<uint>("clock_ratio").required();
    std::string path=param<std::string>("path").required();
    drain=param<uint64_t>("fence_drain_cycles").default_val(64);
    limit=param<uint64_t>("max_cycles").default_val(10000000);
    std::map<std::string,int> types={{"LD",0},{"ST",1},
      {"PIM_MAC_AB",4},{"PIM_MAC_SB",5},{"PIM_MAC_PB",6},
      {"PIM_WR_GB",7},{"PIM_MV_SB",8},{"PIM_MV_GB",9},
      {"PIM_SFM",10},{"PIM_SET_MODEL",11},{"PIM_SET_HEAD",12},
      {"PIM_BARRIER",13},{"FENCE",-1}};
    std::ifstream in(path);
    if(!in) throw ConfigurationError("Cannot open phase trace {}",path);
    std::string line;
    size_t lineno=0;
    while(std::getline(in,line)) {
      ++lineno;
      if(line.empty() || line[0]=='#') continue;
      std::istringstream ss(line);
      std::string name, a, extra;
      if(!(ss>>name>>a) || ss>>extra || !types.count(name))
        throw ConfigurationError("Bad phase trace line {}",lineno);
      size_t n=0;
      auto addr=std::stoll(a,&n,0);
      if(n!=a.size() || addr<0 || (name=="FENCE" && addr!=0))
        throw ConfigurationError("Invalid address on line {}",lineno);
      ops.push_back({types.at(name),addr});
    }
    if(ops.empty()) throw ConfigurationError("Empty phase trace");
    register_stat(sent).name("phase_sent_requests");
    register_stat(fences).name("phase_completed_fences");
    register_stat(stalls).name("phase_fence_stall_cycles");
  }
  void tick() override {
    ++cycles;
    if(cycles>limit) throw std::runtime_error("Phase traffic watchdog exceeded");
    while(pos<ops.size()) {
      auto t=ops[pos];
      if(t.type==-1) {
        ++stalls;
        if(m_memory_system->is_pending()) return;
        if(!draining) {draining=true;drain_until=cycles+drain;}
        if(cycles<drain_until) return;
        draining=false;
        ++fences; ++pos;
        
        return;
      }
      if(!m_memory_system->send({t.addr,t.type})) return;
      ++pos; ++sent;
    }
  }
  bool is_finished() override {return pos==ops.size();}
};
}
