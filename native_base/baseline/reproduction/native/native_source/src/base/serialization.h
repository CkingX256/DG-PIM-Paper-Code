#ifndef     RAMULATOR_BASE_SERIALIZATION_H
#define     RAMULATOR_BASE_SERIALIZATION_H

#include <string>


namespace Ramulator {

    
                                                                        
    
   
template<class T>
class Serializable {
  friend T;

  public:
        
                                                   
        
       
    virtual void serialize() = 0;

        
                                                     
        
       
    virtual void deserialize() = 0;
};  


}        


#endif   