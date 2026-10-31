#ifndef RAMULATOR_DRAM_DRAM_H
#define RAMULATOR_DRAM_DRAM_H

#include <string>
#include <vector>
#include <map>
#include <functional>

#include "base/base.h"
#include "dram/spec.h"
#include "dram/node.h"

namespace Ramulator {

class IDRAM : public Clocked<IDRAM> {
  RAMULATOR_REGISTER_INTERFACE(IDRAM, "DRAM", "DRAM Deivce Model Interface")

                                                    
                                 
                                                      
  public:
    int m_internal_prefetch_size = -1;  
    SpecDef m_levels;                   
    Organization m_organization;        
    int m_channel_width = -1;           


                                                    
                                     
                                                   
  public:
    SpecDef m_commands;                                   
    SpecLUT<Level_t> m_command_scopes{m_commands};        
    SpecLUT<DRAMCommandMeta> m_command_meta{m_commands};  

    SpecDef m_requests;                                     
    SpecLUT<Command_t> m_request_translations{m_requests};  


                                                    
                                
                                                   
  public:
    SpecDef m_states;
    SpecLUT<State_t> m_init_states{m_states};


                                                    
                              
                                                   
  public:
    SpecDef m_timings;                      
    SpecLUT<int> m_timing_vals{m_timings};  

    TimingCons m_timing_cons;           

    Clk_t m_read_latency = -1;          


                                                    
                                        
                                                      
  public:
        
                                                                 
                
                                                                                                 
                                                                                          
        
       
    virtual void issue_command(int command, const AddrVec_t& addr_vec) = 0;

        
                                                                                 
                  
                                                                                    
                                                         
        
       
    virtual int get_preq_command(int command, const AddrVec_t& addr_vec) = 0;

        
                                                                                  
                
                                                                                               
                                                                                
        
       
    virtual bool check_ready(int command, const AddrVec_t& addr_vec) = 0;

        
                                                                            
                
                                                                                    
                                   
        
       
    virtual bool check_rowbuffer_hit(int command, const AddrVec_t& addr_vec) = 0;

        
                                                                                               
                
                                                                                                                         
                                                                                                                     
                                                                                             
        
       
    virtual void notify(std::string_view key, uint64_t value) {};


                                                    
                                           
                                                      
  public:
    int get_level_size(std::string name) {
      try {
        int level_idx = m_levels(name);
        return m_organization.count[level_idx];
      } catch (const std::out_of_range& e) {
        return -1;
      }
    }
};

#define RAMULATOR_DECLARE_SPECS() \
  IDRAM::m_internal_prefetch_size = m_internal_prefetch_size; \
  IDRAM::m_levels = m_levels; \
  IDRAM::m_commands = m_commands; \
  IDRAM::m_command_scopes = m_command_scopes; \
  IDRAM::m_command_meta = m_command_meta; \
  IDRAM::m_command_meta = m_command_meta; \
  IDRAM::m_requests = m_requests; \
  IDRAM::m_request_translations = m_request_translations; \
  IDRAM::m_states = m_states; \
  IDRAM::m_init_states = m_init_states; \
  IDRAM::m_timings = m_timings; \

}        

#endif   

