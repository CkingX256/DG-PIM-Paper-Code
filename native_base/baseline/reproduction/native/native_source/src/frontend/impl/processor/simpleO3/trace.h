#ifndef     RAMULATOR_FRONTEND_PROCESSOR_TRACE_H
#define     RAMULATOR_FRONTEND_PROCESSOR_TRACE_H

#include <vector>

#include "base/type.h"

namespace Ramulator {

    
                                                  
            
                                                                                            
                                                                                 
                                                                       
                                                              
   
class SimpleO3Trace {
  private:
    struct Trace {
      int bubble_count = 0;
      Addr_t load_addr = -1;
      Addr_t store_addr = -1;
    };
    std::vector<Trace> m_trace;

    size_t m_trace_length = 0;
    size_t m_curr_trace_idx = 0;

    int m_load_type;
    int m_store_type;


  public:
    SimpleO3Trace(std::string file_path_str);
    void get_trace(int& bubble_count, Addr_t& load_addr, Addr_t& store_addr);
};

}        


#endif   