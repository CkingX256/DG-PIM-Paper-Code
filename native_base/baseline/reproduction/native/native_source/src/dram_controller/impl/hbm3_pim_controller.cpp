#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"
#include <map>
#include "producer_dma_bridge.h"

namespace Ramulator {

class HBM3PIMController final : public IDRAMController, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAMController, HBM3PIMController, "HBM3-PIM", "A HBM3-PIM controller.");
  private:
    std::deque<Request> pending;          
    
    std::multimap<Clk_t, Request> numeric_pending;
    int numeric_mac_latency = 6;
    int numeric_move_latency = 4;
    IProducerWriteIssue* producer_issue_listener=nullptr;

    ReqBuffer m_active_buffer;            
    ReqBuffer m_priority_buffer;          
    ReqBuffer m_read_buffer;              
    ReqBuffer m_write_buffer;             
    ReqBuffer m_pim_buffer;               

    int m_row_addr_idx = -1;

    float m_wr_low_watermark;
    float m_wr_high_watermark;
    bool  m_is_write_mode = false;

    std::vector<IControllerPlugin*> m_plugins;

    size_t s_num_row_hits = 0;
    size_t s_num_row_misses = 0;
    size_t s_num_row_conflicts = 0;

    bool is_pim = false;

  public:
    void init() override {
      numeric_mac_latency = param<int>("numeric_mac_latency").default_val(6);
      numeric_move_latency = param<int>("numeric_move_latency").default_val(4);
      if (numeric_mac_latency < 1 || numeric_move_latency < 1)
        throw std::runtime_error("Invalid numerical completion latency");
      m_wr_low_watermark =  param<float>("wr_low_watermark").desc("Threshold for switching back to read mode.").default_val(0.2f);
      m_wr_high_watermark = param<float>("wr_high_watermark").desc("Threshold for switching to write mode.").default_val(0.8f);

      m_scheduler = create_child_ifce<IScheduler>();
      m_refresh = create_child_ifce<IRefreshManager>();    

      if (m_config["plugins"]) {
        YAML::Node plugin_configs = m_config["plugins"];
        for (YAML::iterator it = plugin_configs.begin(); it != plugin_configs.end(); ++it) {
          m_plugins.push_back(create_child_ifce<IControllerPlugin>(*it));
        }
      }
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_dram = memory_system->get_ifce<IDRAM>();
      producer_issue_listener=dynamic_cast<IProducerWriteIssue*>(memory_system);
      m_row_addr_idx = m_dram->m_levels("row");
      m_priority_buffer.max_size = 512*3 + 32;
    };

    bool send(Request& req) override {
      req.final_command = m_dram->m_request_translations(req.type_id);

      
      if (req.type_id == Request::Type::Read) {
        auto compare_addr = [req](const Request& wreq) {
          return wreq.addr == req.addr;
        };
        if (std::find_if(m_write_buffer.begin(), m_write_buffer.end(), compare_addr) != m_write_buffer.end()) {
          
          req.depart = m_clk + 1;
          pending.push_back(req);
          return true;
        }
      }

      
      bool is_success = false;
      req.arrive = m_clk;
      switch (req.type_id) {
        case Request::Type::Read:           is_success = m_read_buffer.enqueue(req);  break;
        case Request::Type::Write:          is_success = m_write_buffer.enqueue(req); break;
        case Request::Type::PIM_MAC_AB:     is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_MAC_SB:     is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_MAC_PB:     is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_WR_GB:      is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_MV_SB:      is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_MV_GB:      is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_SFM:        is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_SET_MODEL:  is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_SET_HEAD:   is_success = m_pim_buffer.enqueue(req);   break;
        case Request::Type::PIM_BARRIER:    is_success = m_pim_buffer.enqueue(req);   break;
        default: throw std::runtime_error("Invalid request type!");
      }

      if (!is_success) {
        
        req.arrive = -1;
        return false;
      }

      return true;
    };

    bool priority_send(Request& req) override {
      req.final_command = m_dram->m_request_translations(req.type_id);

      bool is_success = false;
      is_success = m_priority_buffer.enqueue(req);
      return is_success;
    }

    void tick() override {
      m_clk++;

      
      serve_completed_reads();
      while (!numeric_pending.empty() && numeric_pending.begin()->first <= m_clk) {
        auto event = numeric_pending.begin()->second;
        numeric_pending.erase(numeric_pending.begin());
        event.callback(event);
      }

      m_refresh->tick();

      
      ReqBuffer::iterator req_it;
      ReqBuffer* buffer = nullptr;
      bool request_found = schedule_request(req_it, buffer);

      
      for (auto plugin : m_plugins) {
        plugin->update(request_found, req_it);
      }

      
      int first_issued_command = -1;
      if (request_found) {
        first_issued_command = req_it->command;
        
        m_dram->issue_command(req_it->command, req_it->addr_vec);

        
        if (req_it->command == req_it->final_command) {
          if (req_it->type_id == Request::Type::Read) {
            req_it->depart = m_clk + m_dram->m_read_latency;
            pending.push_back(*req_it);
          } else if (req_it->type_id == Request::Type::Write) {
            
          }
          notify_producer_write_issue(*req_it);
          enqueue_numeric_completion(*req_it);
          buffer->remove(req_it);
        } else {
          if (!is_pim) {
            if (m_dram->m_command_meta(req_it->command).is_opening) {
              m_active_buffer.enqueue(*req_it);
              buffer->remove(req_it);
            }
          }
        }
      }

      
      ReqBuffer::iterator sec_req_it;
      ReqBuffer* sec_buffer = nullptr;
      bool sec_request_found = false;
      
      
      sec_request_found = schedule_sec_request(sec_req_it, buffer, first_issued_command);
      for (auto plugin : m_plugins) {
        plugin->update(sec_request_found, sec_req_it);
      }
      if (sec_request_found) {
        m_dram->issue_command(sec_req_it->command, sec_req_it->addr_vec);

        if (sec_req_it->command == sec_req_it->final_command) {
          if (sec_req_it->type_id == Request::Type::Read) {
            sec_req_it->depart = m_clk + m_dram->m_read_latency;
            pending.push_back(*sec_req_it);
          } else if (sec_req_it->type_id == Request::Type::Write) {
          }
          notify_producer_write_issue(*sec_req_it);
          enqueue_numeric_completion(*sec_req_it);
          buffer->remove(sec_req_it);
        } else {
          if (!is_pim) {
            if (m_dram->m_command_meta(sec_req_it->command).is_opening) {
              m_active_buffer.enqueue(*sec_req_it);
              buffer->remove(sec_req_it);
            }
          }
        }
      }
    };


    bool is_pending() override {
      bool is_pending = m_active_buffer.size() ||  m_priority_buffer.size() || m_read_buffer.size() || m_write_buffer.size() || m_pim_buffer.size() || pending.size();
      return is_pending || !numeric_pending.empty();
    };

  private:
    void notify_producer_write_issue(const Request& req){
      if(producer_issue_listener&&req.type_id==Request::Type::Write)producer_issue_listener->producer_write_issued(req,m_clk);
    }
    void enqueue_numeric_completion(const Request& req) {
      if (!req.callback || req.type_id == Request::Type::Read) return;
      Request copy = req;
      int latency = numeric_move_latency;
      if (req.type_id == Request::Type::PIM_MAC_AB ||
          req.type_id == Request::Type::PIM_MAC_SB ||
          req.type_id == Request::Type::PIM_MAC_PB) latency = numeric_mac_latency;
      if (req.type_id == Request::Type::Write)
        latency = m_dram->m_timing_vals("nWR") + m_dram->m_timing_vals("nBL");
      copy.depart = m_clk + latency;
      numeric_pending.emplace(copy.depart, copy);
    }
       
                                                                     
               
                                                                       
                                                                                         
                                                                                                    
       
    void serve_completed_reads() {
      if (pending.size()) {
        
        auto& req = pending[0];
        if (req.depart <= m_clk) {
          
          if (req.depart - req.arrive > 1) {
            
            
          }

          if (req.callback) {
            
            req.callback(req);
          }
          
          pending.pop_front();
        }
      };
    };


       
                                                          
       
       
    void set_write_mode() {
      if (!m_is_write_mode) {
        if ((m_write_buffer.size() > m_wr_high_watermark * m_write_buffer.max_size) || m_read_buffer.size() == 0) {
          m_is_write_mode = true;
        }
      } else {
        if ((m_write_buffer.size() < m_wr_low_watermark * m_write_buffer.max_size) && m_read_buffer.size() != 0) {
          m_is_write_mode = false;
        }
      }
    };


       
                                                                                
       
       
    bool schedule_request(ReqBuffer::iterator& req_it, ReqBuffer*& req_buffer) {
      bool request_found = false;
      
      if (req_it= m_scheduler->get_best_request(m_active_buffer); req_it != m_active_buffer.end()) {
        if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
          request_found = true;
          req_buffer = &m_active_buffer;
        }
      }

      
      if (!request_found) {
        
        if (m_priority_buffer.size() != 0) {
          req_buffer = &m_priority_buffer;
          req_it = m_priority_buffer.begin();
          req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec);
          
          request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
          if (!request_found & m_priority_buffer.size() != 0) {
            return false;
          }
        }

        is_pim = false;
        
        if (!request_found) {
          auto& buffer = m_pim_buffer;
          if (req_it = m_scheduler->get_best_request(buffer); req_it != buffer.end()) {
            request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
            req_buffer = &buffer;
          }
          if (request_found) {
            is_pim = true;
          }
        }


        
        if (!request_found) {
          
          set_write_mode();
          auto& buffer = m_is_write_mode ? m_write_buffer : m_read_buffer;
          if (req_it = m_scheduler->get_best_request(buffer); req_it != buffer.end()) {
            request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
            req_buffer = &buffer;
          }
        }
      }

      
      if (request_found) {
        if (m_dram->m_command_meta(req_it->command).is_closing) {
          std::vector<Addr_t> rowgroup((req_it->addr_vec).begin(), (req_it->addr_vec).begin() + m_row_addr_idx);

          
          for (auto _it = m_active_buffer.begin(); _it != m_active_buffer.end(); _it++) {
            std::vector<Addr_t> _it_rowgroup(_it->addr_vec.begin(), _it->addr_vec.begin() + m_row_addr_idx);
            if (rowgroup == _it_rowgroup) {
              
              request_found = false;
            }
          }
        }
      }

      return request_found;
    }


       
                                                                                       
       
       
    bool schedule_sec_request(ReqBuffer::iterator& req_it, ReqBuffer*& req_buffer, int first_command) {
      bool request_found = false;
      
      if (req_it= m_scheduler->get_best_request(m_active_buffer); req_it != m_active_buffer.end()) {
        if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
          if (compare_command_type(first_command, req_it->command)) {
            request_found = true;
            req_buffer = &m_active_buffer;
          }
        }
      }

      
      if (!request_found) {
        
        if (m_priority_buffer.size() != 0) {
          req_buffer = &m_priority_buffer;
          req_it = m_priority_buffer.begin();
          req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec);
          
          if (compare_command_type(first_command, req_it->command)) {
            request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
            if (!request_found & m_priority_buffer.size() != 0) {
              return false;
            }
          }
        }

        is_pim = false;
        
        if (!request_found) {
          auto& buffer = m_pim_buffer;
          if (req_it = m_scheduler->get_best_request(buffer); req_it != buffer.end()) {
            if (compare_command_type(first_command, req_it->command)) {
              request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
              req_buffer = &buffer;
            }
          }
          if (request_found) {
            is_pim = true;
          }
        }


        
        if (!request_found) {
          
          set_write_mode();
          auto& buffer = m_is_write_mode ? m_write_buffer : m_read_buffer;
          if (req_it = m_scheduler->get_best_request(buffer); req_it != buffer.end()) {
            if (compare_command_type(first_command, req_it->command)) {
              request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
              req_buffer = &buffer;
            }
          }
        }
      }

      
      if (request_found) {
        if (m_dram->m_command_meta(req_it->command).is_closing) {
          std::vector<Addr_t> rowgroup((req_it->addr_vec).begin(), (req_it->addr_vec).begin() + m_row_addr_idx);

          
          for (auto _it = m_active_buffer.begin(); _it != m_active_buffer.end(); _it++) {
            std::vector<Addr_t> _it_rowgroup(_it->addr_vec.begin(), _it->addr_vec.begin() + m_row_addr_idx);
            if (rowgroup == _it_rowgroup) {
              
              request_found = false;
            }
          }
        }
      }

      return request_found;
    }

    
    int get_command_type(int command) {
      if      (command == m_dram->m_commands("ACT"))   return 0; 
      else if (command == m_dram->m_commands("PRE"))   return 0; 
      else if (command == m_dram->m_commands("PREA"))  return 0; 
      else if (command == m_dram->m_commands("RD"))    return 1; 
      else if (command == m_dram->m_commands("WR"))    return 1; 
      else if (command == m_dram->m_commands("REFab")) return 0; 
      else if (command == m_dram->m_commands("REFsb")) return 0; 
      else if (command == m_dram->m_commands("ACTAB")) return 0; 
      else if (command == m_dram->m_commands("ACTSB")) return 0; 
      else if (command == m_dram->m_commands("ACTPB")) return 0; 
      else if (command == m_dram->m_commands("MACAB")) return 1; 
      else if (command == m_dram->m_commands("MACSB")) return 1; 
      else if (command == m_dram->m_commands("MACPB")) return 1; 
      else if (command == m_dram->m_commands("WRGB"))  return 1; 
      else if (command == m_dram->m_commands("MVSB"))  return 1; 
      else if (command == m_dram->m_commands("MVGB"))  return 1; 
      else if (command == m_dram->m_commands("SFM"))   return 1; 
      else if (command == m_dram->m_commands("SETM"))  return 1; 
      else if (command == m_dram->m_commands("SETH"))  return 1; 
      else return -1;
    }

    bool compare_command_type(int first_command, int second_command) {
      int first_command_type = get_command_type(first_command);
      int second_command_type = get_command_type(second_command);
 
      if (first_command_type == -1 || second_command_type == -1) {
        return false;
      }

      if (first_command_type == second_command_type) {
        return false;
      }
      else {
        return true;
      }
    }
};
  
}   
