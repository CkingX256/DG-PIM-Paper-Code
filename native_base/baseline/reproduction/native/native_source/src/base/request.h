#ifndef     RAMULATOR_BASE_REQUEST_H
#define     RAMULATOR_BASE_REQUEST_H

#include <vector>
#include <list>
#include <string>

#include "base/base.h"

namespace Ramulator {

struct Request { 
  Addr_t    addr = -1;
  AddrVec_t addr_vec {};

  
  
  struct Type {
    enum : int {
      
      Read = 0, 
      Write = 1,

      
      PIM_MAC_AB = 4,
      PIM_MAC_SB = 5,
      PIM_MAC_PB = 6,
      PIM_WR_GB = 7,
      PIM_MV_SB = 8,
      PIM_MV_GB = 9,
      PIM_SFM = 10,
      PIM_SET_MODEL = 11,
      PIM_SET_HEAD = 12,
      PIM_BARRIER = 13
    };
  };

  int type_id = -1;    
  int source_id = -1;  

  int command = -1;          
  int final_command = -1;    

  Clk_t arrive = -1;   
  Clk_t depart = -1;   

  std::function<void(Request&)> callback;

  Request(Addr_t addr, int type);
  Request(AddrVec_t addr_vec, int type);
  Request(Addr_t addr, int type, int source_id, std::function<void(Request&)> callback);
};


struct ReqBuffer {
  std::list<Request> buffer;
  size_t max_size = 32;


  using iterator = std::list<Request>::iterator;
  iterator begin() { return buffer.begin(); };
  iterator end() { return buffer.end(); };


  size_t size() const { return buffer.size(); }

  bool enqueue(const Request& request) {
    if (buffer.size() <= max_size) {
      buffer.push_back(request);
      return true;
    } else {
      return false;
    }
  }

  void remove(iterator it) {
    buffer.erase(it);
  }
};

}        


#endif   