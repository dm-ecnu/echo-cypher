// One fixed DBLP-only calibration panel; no KS, MQO or LDBC inputs.
#include "echo_common.h"
#include "echo_dpiso.h"
#include "echo_envelope.h"
#include "echo_search.h"
#include "../vendor/nlohmann/json.hpp"
#include <set>
#include <thread>
using namespace echo;
int main(int argc,char**argv){
 auto a=parse_args(argc,argv);auto data=load_data_graph(a.get("graph"));
 uint32_t maxlabel=0;for(uint32_t v=0;v<data->getVerticesCount();++v)maxlabel=std::max(maxlabel,data->getVertexLabel(v));
 std::vector<uint64_t> freq(maxlabel+2,1);for(uint32_t v=0;v<data->getVerticesCount();++v)++freq[data->getVertexLabel(v)];
 auto runbase=[&](const QueryGraph&q,std::vector<std::vector<uint32_t>>&sol){auto g=to_base_query(q);LfrtControl ctl;ctl.deadline=Clock::now()+std::chrono::seconds(120);auto r=dpiso_run(data,g,&sol,&ctl);delete g;if(r.timed_out)throw std::runtime_error("calibration base timed out");return r;};
 for(int rep=0;rep<5;++rep)for(int overlap:{9,12}){
  auto w=read_workload_manifest(a.get("workloads")+"/dblp-100-k15-b30-o"+std::to_string(overlap)+"-s0");
  for(int batch=0;batch<5;++batch){
   std::vector<QueryGraph> qs;for(uint32_t i=w.batch_start(batch);i<w.batch_start(batch)+w.batch_count(batch);++i)qs.push_back(w.read(i));
   std::vector<std::vector<uint32_t>> fams;std::thread planner([&]{fams=families_of_min(qs,data,0,0);});planner.join();
   for(auto& fam:fams){if(fam.size()<2)continue;auto env=make_core_envelope(qs,fam,data,2);if(!env.n)continue;
    uint64_t estimate=0;for(auto q:fam)for(auto label:qs[q].label)estimate+=freq[label];
    std::vector<std::vector<std::vector<uint32_t>>> truth(fam.size());double separate=0,filter=0;EnvRun merged;
    auto singles=[&]{for(size_t t=0;t<fam.size();++t){auto r=runbase(qs[fam[t]],truth[t]);separate+=r.filter_sec+r.order_sec+r.enum_sec;filter+=r.filter_sec;}};
    auto joint=[&]{merged=run_envelope(qs,data,env,{},0,nullptr,Clock::now()+std::chrono::seconds(120));if(merged.timed_out)throw std::runtime_error("calibration walk timed out");};
    if(rep%2){joint();singles();}else{singles();joint();}
    if(merged.per_query.size()!=fam.size())merged.per_query.resize(fam.size());
    // The N6 reference is already fully enumerated. Complete a recorded prefix by
    // selecting its exact reference extensions, the same operation used by the
    // main runner for large prefix sets; do not re-filter the data per prefix.
    for(auto& departure:merged.depart){int t=departure.first;
      for(const auto& sol:truth[t]){bool accepted=false;
        for(const auto& prefix:departure.second){bool agrees=true;for(auto& x:prefix)if(sol[x.first]!=x.second){agrees=false;break;}if(agrees){accepted=true;break;}}
        if(accepted)merged.per_query[t].push_back(sol);
      }
    }
    for(size_t t=0;t<fam.size();++t){std::multiset<std::vector<uint32_t>> x(truth[t].begin(),truth[t].end()),y(merged.per_query[t].begin(),merged.per_query[t].end());if(x!=y)throw std::runtime_error("calibration set-equivalence failed");}
    nlohmann::json j={{"rep",rep},{"overlap",overlap},{"batch",batch},{"family",fam},{"est_split",estimate},{"separate_sec",separate},{"separate_filter_sec",filter},{"walk_sec",merged.search_sec},{"n4_sec",merged.filter_sec},{"actual_candidates",merged.cell_sum},{"equivalent",true}};std::cout<<j.dump()<<std::endl;
   }
  }
 }
 delete data;
}
