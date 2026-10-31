#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using I=long long;using K=std::array<I,3>;using Iv=std::array<I,2>;
void ck(bool b,const char*m){if(!b)throw std::runtime_error(m);}
std::vector<std::string> split(const std::string&s){std::vector<std::string>x;std::istringstream f(s);std::string z;while(std::getline(f,z,'\t'))x.push_back(z);return x;}
std::vector<Iv> merged(std::vector<Iv>v){std::sort(v.begin(),v.end());std::vector<Iv>o;for(auto a:v)if(o.empty()||a[0]>o.back()[1])o.push_back(a);else o.back()[1]=std::max(o.back()[1],a[1]);return o;}
I intersection(const std::vector<Iv>&a,const std::vector<Iv>&b){I sum=0;size_t i=0,j=0;while(i<a.size()&&j<b.size()){sum+=std::max<I>(0,std::min(a[i][1],b[j][1])-std::max(a[i][0],b[j][0]));if(a[i][1]<b[j][1])++i;else++j;}return sum;}
int main(int argc,char**argv){try{
 ck(argc==2,"usage audit_pipeline RUN_DIR");std::filesystem::path p=argv[1];
 struct Batch {I slot=-1;std::map<std::string,I>e;I mac=0,move=0;};
 std::map<K,Batch>batches;std::set<std::array<I,2>>heads;std::vector<Iv>epu,mac_intervals,move_intervals;
 std::ifstream f(p/"qpipe_events.tsv");ck(bool(f),"pipeline log missing");std::string line;std::getline(f,line);
 while(std::getline(f,line)){auto a=split(line);ck(a.size()==8,"bad pipeline event");I cycle=std::stoll(a[0]),q=std::stoll(a[1]),h=std::stoll(a[2]),base=std::stoll(a[3]),slot=std::stoll(a[4]);ck(base%16==0&&slot>=0&&slot<2,"bad slot/base");K k{q,h,base};auto&b=batches[k];if(b.slot<0)b.slot=slot;ck(b.slot==slot,"slot identity changed");ck(!b.e.count(a[5]),"duplicate pipeline transition");b.e[a[5]]=cycle;heads.insert({q,h});}
 std::map<K,std::vector<Iv>>lives;I max_exp_jobs=0;
 for(auto&[k,b]:batches){
  for(const char*n:{"acquire","mac_end","readout_begin","readout_end","max_begin","max_end"})ck(b.e.count(n),"incomplete batch protocol");
  ck(b.e["acquire"]<=b.e["mac_end"]&&b.e["mac_end"]<=b.e["readout_begin"]&&b.e["readout_begin"]<b.e["readout_end"]&&b.e["readout_end"]<=b.e["max_begin"]&&b.e["max_begin"]<b.e["max_end"],"noncausal pipeline ordering");
  I end=b.e["max_end"];epu.push_back({b.e["max_begin"],end});
  if(b.e.count("exp_begin")){ck(b.e.count("exp_end"),"unfinished exponent service");ck(b.e["exp_begin"]==end&&b.e["exp_end"]>end,"bad exponent service");end=b.e["exp_end"];epu.push_back({b.e["exp_begin"],end});++max_exp_jobs;}
  lives[{k[0],k[1],b.slot}].push_back({b.e["acquire"],end});
 }
 for(auto&[k,v]:lives){std::sort(v.begin(),v.end());for(size_t i=1;i<v.size();i++)ck(v[i-1][1]<=v[i][0],"overwritten live accumulator");}
 std::ifstream ev(p/"events.tsv");ck(bool(ev),"events missing");std::getline(ev,line);
 while(std::getline(ev,line)){auto a=split(line);ck(a.size()==11,"bad native event");I q=std::stoll(a[4]),h=std::stoll(a[5]),op=std::stoll(a[6]);if(!heads.count({q,h})||(op!=6&&op!=7))continue;
  I issue=std::stoll(a[2]),end=std::stoll(a[3]),tag=std::stoll(a[10]),base=tag/2;K k{q,h,base};ck(batches.count(k),"callback without batch selector");auto&b=batches.at(k);ck(b.slot==(tag&1),"micro-op accumulator selector mismatch");
  if(op==6){I g=std::stoll(a[8]);ck(g>=base&&g<base+16,"MAC selector/group mismatch");ck(issue>=b.e["acquire"]&&end<=b.e["mac_end"],"MAC outside owned lifetime");++b.mac;mac_intervals.push_back({issue,end});}
  else{ck(issue>=b.e["readout_begin"]&&end<=b.e["readout_end"]&&issue>=b.e["mac_end"],"readout before MAC closure");++b.move;move_intervals.push_back({issue,end});}
 }
 auto eu=merged(epu),ma=merged(mac_intervals),mo=merged(move_intervals);ck(intersection(eu,mo)==0,"score readout and EPU port overlap");I nm=0,nr=0;
 for(auto&[k,b]:batches){ck(b.mac>0&&b.move>0,"missing physical callbacks");nm+=b.mac;nr+=b.move;}
 std::cout<<"{\"status\":\"PASS_NATIVE_ACCUMULATOR_OWNERSHIP_AND_SCORE_PORT\",\"heads\":"<<heads.size()<<",\"batches\":"<<batches.size()<<",\"qk_callbacks\":"<<nm<<",\"readout_callbacks\":"<<nr<<",\"exp_jobs\":"<<max_exp_jobs<<",\"physical_mac_epu_overlap_cycles\":"<<intersection(eu,ma)<<",\"physical_readout_epu_overlap_cycles\":0}"<<'\n';
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
