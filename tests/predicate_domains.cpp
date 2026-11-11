#include "echo_common.h"
#include "echo_dpiso.h"
#include "echo_envelope.h"
#include "echo_search.h"
#include "masc.h"
#include <set>
#include <cassert>
using namespace echo;
int main(int argc,char**argv){
 auto d=load_data_graph(argv[1]); QueryGraph q; q.n=11;q.label.assign(11,1);q.adj.resize(11);
 for(int i=0;i<10;++i){q.adj[i].push_back(i+1);q.adj[i+1].push_back(i);}
 std::vector<QueryGraph> qs(5,q);
 for(int t=0;t<5;++t){PredicateDomain domain;domain.allowed.resize(11);domain.constrained.assign(11,0);domain.constrained[0]=t!=3;if(t!=4)domain.allowed[0]={(uint32_t)t};qs[t].domain_id=store_predicate_domain(std::move(domain));}
 std::vector<std::vector<std::vector<uint32_t>>> truth(5);
 for(int t=0;t<5;++t){auto g=to_base_query(qs[t]);dpiso_run(d,g,&truth[t]);delete g;for(auto& sol:truth[t])assert(qs[t].permits(0,sol[0]));}
 auto env=make_core_envelope(qs,{0,1,2,3,4},d,2);assert(env.n>0);
 for(int private_case: {0,1}) {
 auto tested_env=env;
 if(private_case) {
  PredicateDomain dom=*qs[4].predicates(); dom.constrained[10]=1;dom.allowed[10].clear();qs[4].domain_id=store_predicate_domain(std::move(dom));
  uint32_t code=(4u<<16)|10u;
  for(auto& span:tested_env.span_v) {auto it=std::find(span.begin(),span.end(),code);if(it!=span.end()){span.erase(it);break;}}
  tested_env.span_v.push_back({code});++tested_env.n;
 }
 for(int mode: {0,2}){
  auto got=run_envelope(qs,d,tested_env,{},mode,nullptr,Clock::time_point::max());
  // Resolve departures through the same constrained base entry used in N6.
  for(auto& dep:got.depart){int t=dep.first;for(auto& prefix:dep.second){std::vector<std::pair<int,uint32_t>> pins;for(auto& x:prefix)pins.push_back({(int)x.first,x.second});auto g=to_base_query(qs[t]);std::vector<std::vector<uint32_t>> sols;dpiso_run_pinned(d,g,pins,&sols,nullptr);delete g;got.per_query[t].insert(got.per_query[t].end(),sols.begin(),sols.end());}}
  for(int t=0;t<5;++t){std::multiset<std::vector<uint32_t>> a(truth[t].begin(),truth[t].end()),b(got.per_query[t].begin(),got.per_query[t].end());assert(a==b);}
 }
 }
 LabelIndex index;index.build(d);std::vector<std::vector<std::vector<uint32_t>>> cand;signature_filter(d,index,qs,cand,nullptr);MascOptions opt;opt.collect=true;auto got=masc_run(d,qs,cand,opt,Clock::time_point::max(),nullptr,&index);
 for(int t=0;t<5;++t){std::multiset<std::vector<uint32_t>> a(truth[t].begin(),truth[t].end()),b(got.per_query[t].begin(),got.per_query[t].end());assert(a==b);}
 for(int t=0;t<5;++t){PredicateDomain dom;dom.allowed.resize(11);dom.constrained.assign(11,0);dom.constrained[0]=1;qs[t].domain_id=store_predicate_domain(std::move(dom));}
 auto empty=run_envelope(qs,d,env,{},0,nullptr,Clock::time_point::max());assert(empty.per_query.size()==5);for(auto& sol:empty.per_query)assert(sol.empty());
 std::cout<<"predicate domains: differing anchors, absent/empty sets, shared masks and MASC reuse passed\n";
}
