#ifndef     RAMULATOR_BASE_LOGGING_H
#define     RAMULATOR_BASE_LOGGING_H

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <yaml-cpp/yaml.h>

#include <vector>
#include <string>

#include "base/exception.h"


namespace Ramulator {

using Logger_t = std::shared_ptr<spdlog::logger>;

class Logging {
  private:
    inline static const std::string default_logger_pattern = "[%n] %^[%l]%$ %v";

  public:
        
                                             
        
                                           
                         
       
    static Logger_t create_logger(std::string name, std::string pattern = default_logger_pattern);

        
                                                                 
        
                    
                         
       
    static Logger_t get(std::string name);

  private:
    static bool _create_base_logger();
    inline static bool base_logger_registered = _create_base_logger();
    
  public:
    Logging() = delete;
    Logging(const Logging&) = delete;
    void operator=(const Logging&) = delete;
    Logging(Logging&&) = delete;
};


}        


#endif   