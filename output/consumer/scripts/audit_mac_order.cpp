#include <array>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>


int main(int argc,char**argv){try{
 if(argc!=2)throw std::runtime_error("usage: audit_mac_order D < events.tsv");
 int tiles=(std::stoi(argv[1])+63)/64;std::string line;std::getline(std::cin,line);
 std::map<std::array<int,3>,int>next;long long calls=0;
 while(std::getline(std::cin,line)){
  std::istringstream in(line);std::vector<long long>a;long long x;while(in>>x)a.push_back(x);
  if(a.size()!=11)throw std::runtime_error("invalid callback row");
  if(a[6]!=6)continue;
  auto k=std::array<int,3>{int(a[4]),int(a[5]),int(a[8])};
  if(a[9]!=next[k]++)throw std::runtime_error("changed per-group FMA tile order");++calls;
 }
 for(auto[k,n]:next)if(n!=tiles)throw std::runtime_error("incomplete QK dimension chain");
 std::cout<<"{\"status\":\"PASS_ACTUAL_QK_FMA_TILE_ORDER\",\"groups\":"<<next.size()<<",\"callbacks\":"<<calls<<"}\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
