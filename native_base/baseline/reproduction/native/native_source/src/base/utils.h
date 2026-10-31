#ifndef RAMULATOR_BASE_UTILS_H
#define RAMULATOR_BASE_UTILS_H

#include <string>
#include <vector>
#include <cstdint>

namespace Ramulator {

                                                  
                                             
                                                    

    
                                                                            
   
                                                                     
                                                 
   
size_t parse_capacity_str(std::string size_str);

    
                                                               
   
                                                                         
                                                 
   
size_t parse_frequency_str(std::string size_str);

    
                                                                                                          
    
                                                     
                                               
                                     
   
uint64_t JEDEC_rounding(float t_ns, int tCK_ps);


    
                                                                                                               
    
                                                     
                                               
                                     
   
uint64_t JEDEC_rounding_DDR5(float t_ns, int tCK_ps);


                                                  
                                         
                                                    

    
                                                          
    
                       
               
                       
   
template <typename Integral_t>
Integral_t calc_log2(Integral_t val) {
  static_assert(std::is_integral_v<Integral_t>, "Only integral types are allowed for bitwise operations!");

  Integral_t n = 0;
  while ((val >>= 1)) {
    n ++;
  }
  return n;
};

    
                                                                                                                     
    
                       
                
                    
                       
   
template <typename Integral_t>
Integral_t slice_lower_bits(Integral_t& addr, int num_bits) {
  static_assert(std::is_integral_v<Integral_t>, "Only integral types are allowed for bitwise operations!");

  Integral_t lbits = addr & ((1<<num_bits) - 1);
  addr >>= num_bits;
  return lbits;
};


                                                  
                               
                                                    
void tokenize(std::vector<std::string>& tokens, std::string line, std::string delim);

}           

#endif      
