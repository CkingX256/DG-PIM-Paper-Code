#ifndef     RAMULATOR_BASE_Clocked_H
#define     RAMULATOR_BASE_Clocked_H

#include <vector>
#include <string>

#include "base/type.h"

namespace Ramulator {

    
                                                                          
    
                
   
template<class T>
class Clocked {
  friend T;

  protected:
    Clk_t m_clk = 0;

  public:
    virtual void tick() = 0;

  public:
    Clocked() {};
};

}        


#endif   