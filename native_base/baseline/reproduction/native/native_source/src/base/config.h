#ifndef RAMULATOR_BASE_CONFIG_H
#define RAMULATOR_BASE_CONFIG_H

#include <filesystem>
#include <iostream>
#include <regex>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "base/base.h"


namespace Ramulator {
namespace Config {

    
                                                                            
   
                                                   
                                                                       
   
YAML::Node parse_config_file(const std::string& path, const std::vector<std::string>& params);


namespace Details {

    
                                                                                         
    
                                                   
                         
   
YAML::Node load_config_file(const std::string& path_str);

    
                                                                         
   
                                                   
                                                                              
   
void resolve_included_configs(YAML::Node node);

    
                                                                                                       
   
                                                        
                                                                  
   
void override_configs(YAML::Node config, const std::vector<std::string>& params);

}    
}    


}        

#endif   
