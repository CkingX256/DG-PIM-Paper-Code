#ifndef RAMULATOR_DRAM_NODE_H
#define RAMULATOR_DRAM_NODE_H

#include <vector>
#include <map>
#include <deque>
#include <functional>
#include <concepts>

#include "base/type.h"
#include "dram/spec.h"

namespace Ramulator {

class IDRAM;

template<typename T>
concept IsDRAMSpec = requires(T t) { 
  typename T::Node; 
};








    
                                                            
    
   
template<IsDRAMSpec T>
struct DRAMNodeBase {
    using NodeType = typename T::Node;
    NodeType* m_parent_node = nullptr;
    std::vector<NodeType*> m_child_nodes;

    T* m_spec = nullptr;

    int m_level = -1;      
    int m_node_id = -1;    
    int m_size = -1;       

    int m_state = -1;      

    std::vector<Clk_t> m_cmd_ready_clk;             
    std::vector<std::deque<Clk_t>> m_cmd_history;   

    using RowId_t = int;
    using RowState_t = int;
    std::map<RowId_t, RowState_t> m_row_state;  

    DRAMNodeBase(T* spec, NodeType* parent, int level, int id):
    m_spec(spec), m_parent_node(parent), m_level(level), m_node_id(id) {
      int num_cmds = T::m_commands.size();
      m_cmd_ready_clk.resize(num_cmds, -1);
      m_cmd_history.resize(num_cmds);
      for (int cmd = 0; cmd < num_cmds; cmd++) {
        int window = 0;
        for (const auto& t : spec->m_timing_cons[level][cmd]) {
          window = std::max(window, t.window);
        }
        if (window != 0) {
          m_cmd_history[cmd].resize(window, -1);
        } else {
          m_cmd_history[cmd].clear();
        }
      }

      m_state = spec->m_init_states[m_level];

      
      int next_level = level + 1;
      int last_level = T::m_levels["row"];
      if (next_level == last_level) {
        return;
      } else {
        int next_level_size = m_spec->m_organization.count[next_level];
        if (next_level_size == 0) {
          return;
        } else {
          for (int i = 0; i < next_level_size; i++) {
            NodeType* child = new NodeType(spec, static_cast<NodeType*>(this), next_level, i);
            static_cast<NodeType*>(this)->m_child_nodes.push_back(child);
          }
        }
      }
    };

    void update_states(int command, const AddrVec_t& addr_vec, Clk_t clk) {
      int child_id = addr_vec[m_level+1];
      if (m_spec->m_actions[m_level][command]) {
        
        m_spec->m_actions[m_level][command](static_cast<NodeType*>(this), command, child_id, clk); 
      }
      if (m_level == m_spec->m_command_scopes[command] || !m_child_nodes.size()) {
        
        return; 
      }
      
      m_child_nodes[child_id]->update_states(command, addr_vec, clk);
    };

    void update_timing(int command, const AddrVec_t& addr_vec, Clk_t clk) {
                                                        
                                            
                                                       
      if (m_node_id != addr_vec[m_level]) {
        for (const auto& t : m_spec->m_timing_cons[m_level][command]) {
          if (!t.sibling) {
            
            continue; 
          }

          
          Clk_t future = clk + t.val;
          m_cmd_ready_clk[t.cmd] = std::max(m_cmd_ready_clk[t.cmd], future); 
        }
        
        return; 
      }

                                                        
                                            
                                                       
      
      if (m_cmd_history[command].size()) {
        m_cmd_history[command].pop_back();
        m_cmd_history[command].push_front(clk); 
      }

      for (const auto& t : m_spec->m_timing_cons[m_level][command]) {
        if (t.sibling) {
          continue; 
        }

        
        Clk_t past = m_cmd_history[command][t.window-1];
        if (past < 0) {
          
          continue; 
        }

        
        Clk_t future = past + t.val;
        m_cmd_ready_clk[t.cmd] = std::max(m_cmd_ready_clk[t.cmd], future);
      }

      if (!m_child_nodes.size()) {
        
        return; 
      }

      
      for (auto child : m_child_nodes) {
        child->update_timing(command, addr_vec, clk);
      }
    };

    int get_preq_command(int command, const AddrVec_t& addr_vec, Clk_t m_clk) {
      int child_id = addr_vec[m_level + 1];
      if (m_spec->m_preqs[m_level][command]) {
        int preq_cmd = m_spec->m_preqs[m_level][command](static_cast<NodeType*>(this), command, child_id, m_clk);
        if (preq_cmd != -1) {
          
          return preq_cmd; 
        }
      }

      if (child_id < 0 || !m_child_nodes.size()) {
        
        return command; 
      }

      
      return m_child_nodes[child_id]->get_preq_command(command, addr_vec, m_clk);
    };

    bool check_ready(int command, const AddrVec_t& addr_vec, Clk_t clk) {
      if (m_cmd_ready_clk[command] != -1 && clk < m_cmd_ready_clk[command]) {
        
        return false; 
      }

      int child_id = addr_vec[m_level+1];
      if (child_id < 0 || m_level == m_spec->m_command_scopes[command] || !m_child_nodes.size()) {
        
        return true; 
      }

      
      return m_child_nodes[child_id]->check_ready(command, addr_vec, clk);
    };

    bool check_rowbuffer_hit(int command, const AddrVec_t& addr_vec, Clk_t m_clk) {
      
      int child_id = addr_vec[m_level+1];
      if (m_spec->m_rowhits[m_level][command]) {
        
        return m_spec->m_rowhits[m_level][command](static_cast<NodeType*>(this), command, child_id, m_clk);  
      }

      if (child_id < 0 || !m_child_nodes.size()) {
        
        return false; 
      }

      
      return m_child_nodes[child_id]->check_rowbuffer_hit(command, addr_vec, m_clk);
    };    
};

template<class T>
using ActionFunc_t = std::function<void(typename T::Node* node, int cmd, int target_id, Clk_t clk)>;
template<class T>
using PreqFunc_t   = std::function<int (typename T::Node* node, int cmd, int target_id, Clk_t clk)>;
template<class T>
using RowhitFunc_t = std::function<bool(typename T::Node* node, int cmd, int target_id, Clk_t clk)>;
template<class T>
using RowopenFunc_t = RowhitFunc_t<T>;

template<typename T>
using FuncMatrix = std::vector<std::vector<T>>;







}        

#endif   
