#include <vector>

#include "base/base.h"
#include "dram/dram.h"
#include "addr_mapper/addr_mapper.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

class HBM3PIMLinearMapperBase : public IAddrMapper {
  public:
    IDRAM* m_dram = nullptr;

    int m_num_levels = -1;          
    std::vector<int> m_addr_bits;   
    Addr_t m_tx_offset = -1;

    int m_col_bits_idx = -1;
    int m_row_bits_idx = -1;


  protected:
    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) {
      m_dram = memory_system->get_ifce<IDRAM>();

      
      const auto& count = m_dram->m_organization.count;
      m_num_levels = count.size();
      m_addr_bits.resize(m_num_levels);
      for (size_t level = 0; level < m_addr_bits.size(); level++) {
        m_addr_bits[level] = calc_log2(count[level]);
      }

      
      

      int tx_bytes = m_dram->m_internal_prefetch_size * m_dram->m_channel_width / 8;
      m_tx_offset = calc_log2(tx_bytes);

      
      try {
        m_row_bits_idx = m_dram->m_levels("row");
      } catch (const std::out_of_range& r) {
        throw std::runtime_error(fmt::format("Organization \"row\" not found in the spec, cannot use linear mapping!"));
      }

      
      m_col_bits_idx = m_num_levels - 1;
    }

};


class HBM3PIMMap final : public HBM3PIMLinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, HBM3PIMMap, "HBM3-PIM", "Applies a HBM3-PIM mapping to the address. (CH pCH Ra BG Ba Ro Co)");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      HBM3PIMLinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      req.addr_vec[6] = slice_lower_bits(addr, m_addr_bits[6]); 
      req.addr_vec[5] = slice_lower_bits(addr, m_addr_bits[5]); 
      req.addr_vec[4] = slice_lower_bits(addr, m_addr_bits[4]); 
      req.addr_vec[3] = slice_lower_bits(addr, m_addr_bits[3]); 
      req.addr_vec[2] = slice_lower_bits(addr, m_addr_bits[2]); 
      req.addr_vec[1] = slice_lower_bits(addr, m_addr_bits[1]); 
      req.addr_vec[0] = slice_lower_bits(addr, m_addr_bits[0]); 
    }
};


class HBM3PIMCustomMap final : public HBM3PIMLinearMapperBase, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IAddrMapper, HBM3PIMCustomMap, "HBM3-PIM-Custom", "Applies a HBM3-PIM custom mapping to the address. (Pch Ra Ba Ro Co Ch)");

  public:
    void init() override { };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      HBM3PIMLinearMapperBase::setup(frontend, memory_system);
    }

    void apply(Request& req) override {
      req.addr_vec.resize(m_num_levels, -1);
      Addr_t addr = req.addr >> m_tx_offset;
      req.addr_vec[0] = slice_lower_bits(addr, m_addr_bits[0]);
      for (int i = m_addr_bits.size() - 1; i >= 1; i--) {
        req.addr_vec[i] = slice_lower_bits(addr, m_addr_bits[i]);
      }
    }
};



}   
