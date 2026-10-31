#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
using Key=std::array<long long,9>;
void require(bool x,const std::string& s){if(!x)throw std::runtime_error(s);}
std::vector<std::string> split(const std::string& s,char c){std::istringstream f(s);std::string x;std::vector<std::string> a;while(std::getline(f,x,c))a.push_back(x);return a;}
int main(int argc,char**argv){try{
 require(argc==2,"usage audit_commands RUN_DIR");std::filesystem::path r=argv[1];
 std::map<std::string,int> op{{"RD",1},{"WR",2},{"WRGB",3},{"MACAB",4},{"MVSB",5},{"SFM",6},{"MVGB",7}};
 std::array<std::string,28> code{"RD","WR","RD","WR","WR","WRGB","MACAB","MVSB","SFM","MVGB","MACAB","MVSB","MVSB","RD","RD","RD","RD","WR","WR","RD","RD","WR","WR","RD","RD","WR","WR","WR"};
 std::vector<Key> ev,native;std::unordered_set<long long> ids;std::map<std::string,long long> counts;long long records=0;
 std::ifstream f(r/"events.tsv");require(bool(f),"events missing");std::string line;std::getline(f,line);
 while(std::getline(f,line)){auto a=split(line,'\t');require(a.size()==11,"bad event row");std::array<long long,11>x;for(int i=0;i<11;i++)x[i]=std::stoll(a[i]);require(ids.insert(x[0]).second,"duplicate callback id");require(x[1]<=x[2]&&x[2]<x[3],"invalid callback lifetime");require(x[6]>=0&&x[6]<28,"unknown callback op");unsigned long long ad=x[7];ad>>=5;long long col=ad&31;ad>>=5;long long row=ad&16383;ad>>=14;long long bank=ad&3;ad>>=2;long long bg=ad&3;ad>>=2;long long rank=ad&1;ad>>=1;long long pc=ad&1;ad>>=1;long long ch=ad&15;ev.push_back({x[2],op.at(code[x[6]]),ch,pc,rank,bg,bank,row,col});}
 for(const auto& p:std::filesystem::directory_iterator(r)){if(p.path().filename().string().rfind("commands.log.ch",0)!=0)continue;std::ifstream cf(p.path());while(std::getline(cf,line)){auto a=split(line,',');require(a.size()==9,"bad command row");std::string name=a[1];name.erase(std::remove_if(name.begin(),name.end(),[](unsigned char c){return std::isspace(c);}),name.end());++counts[name];++records;if(!op.count(name))continue;Key k;k[0]=std::stoll(a[0]);k[1]=op.at(name);for(int i=2;i<9;i++)k[i]=std::stoll(a[i]);native.push_back(k);}}
 std::sort(ev.begin(),ev.end());std::sort(native.begin(),native.end());require(ev==native,"callback/native physical multiset differs");
 std::cout<<"{\"status\":\"PASS\",\"events\":"<<ev.size()<<",\"native_records\":"<<records<<",\"commands\":{";bool first=true;for(auto [n,c]:counts){if(!first)std::cout<<',';first=false;std::cout<<'"'<<n<<"\":"<<c;}std::cout<<"},\"equality\":\"sorted full physical tuple and clock, exact multiplicity\"}\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
