#ifndef RAMULATOR_CONTROLLER_CONTROLLER_H
#define RAMULATOR_CONTROLLER_CONTROLLER_H

#include <vector>
#include <deque>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "base/base.h"
#include "dram/dram.h"
#include "dram_controller/scheduler.h"
#include "dram_controller/plugin.h"
#include "dram_controller/refresh.h"


namespace Ramulator {

class IDRAMController : public Clocked<IDRAMController> {
  RAMULATOR_REGISTER_INTERFACE(IDRAMController, "Controller", "Memory Controller Interface");

  public:
    IDRAM*  m_dram = nullptr;          
    IScheduler*   m_scheduler = nullptr;
    IRefreshManager*   m_refresh = nullptr;

    int m_channel_id = -1;
  public:
        
                                                             
        
                                                        
                                        
                                                        
       
    virtual bool send(Request& req) = 0;

        
                                                                           
        
       
    virtual bool priority_send(Request& req) = 0;

        
                                                 
        
       
    virtual void tick() = 0;
   
    virtual bool is_pending() = 0;

    
    Clk_t get_clk() {return m_clk;}

};

}       

#endif  