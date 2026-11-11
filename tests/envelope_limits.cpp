#include "echo_common.h"
#include "echo_dpiso.h"
#include "echo_envelope.h"
#include "echo_search.h"
#include <cassert>
using namespace echo;
int main(int argc,char**argv){
 auto data=load_data_graph(argv[1]);QueryGraph q;q.n=2;q.label={1,1};q.adj={{1},{0}};std::vector<QueryGraph> qs(5,q);
 auto env=make_core_envelope(qs,{0,1,2,3,4},data,1);assert(env.n==2);
 EnvLimits growth;growth.max_candidates=2;auto a=run_envelope(qs,data,env,{},0,nullptr,Clock::now()+std::chrono::seconds(120),growth);
 assert(a.candidate_exceeded && a.cell_sum==2000 && a.nodes==0 && a.solutions==0 && a.search_sec==0 && !a.timed_out);
 EnvLimits attempt;attempt.attempt_seconds=0;auto c=run_envelope(qs,data,env,{},0,nullptr,Clock::now()+std::chrono::seconds(120),attempt);
 assert(c.budget_exceeded && c.filter_budget_exceeded && !c.timed_out && c.search_sec==0 && c.nodes==0 && c.solutions==0);
 for(auto& sols:c.per_query)assert(sols.empty());
 EnvLimits time;time.walk_seconds=.005;auto b=run_envelope(qs,data,env,{},0,nullptr,Clock::now()+std::chrono::seconds(120),time);
 assert(b.budget_exceeded && !b.timed_out && b.discarded_solutions>0 && b.solutions==0 && b.depart.empty());
 for(auto& sols:b.per_query)assert(sols.empty());
 assert(b.search_sec<.055); // bounded cooperative checks, plus scheduler slack
 uint64_t recovered=0;for(auto& query:qs){auto base=to_base_query(query);auto r=dpiso_run(data,base,nullptr);assert(!r.timed_out && r.embeddings==999000);recovered+=r.embeddings;delete base;}
 assert(recovered==4995000);std::cout<<"growth stops before N5; timed walk discards "<<b.discarded_solutions<<" partial results; N6 recovers all "<<recovered<<" exact embeddings\n";delete data;
}
