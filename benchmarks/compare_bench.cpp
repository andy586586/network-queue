
#include "lf_queue.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
using Clock=std::chrono::steady_clock;

static inline void spin_us(int us){
    if(us<=0) return;
    auto end=Clock::now()+std::chrono::microseconds(us);
    while(Clock::now()<end) asm volatile("" ::: "memory");
}
static double pct(std::vector<double> v,double q){
    std::sort(v.begin(),v.end());
    return v[std::min(v.size()-1,(size_t)(q*(v.size()-1)))];
}
struct Res{double ms,tps,p50,p95,p99; uint64_t steals;};

struct GW{
    std::deque<size_t> q; std::mutex m; std::condition_variable cv;
    std::vector<std::thread> ts; std::atomic<bool> running{true}; std::atomic<uint64_t> done{0};
    std::vector<Clock::time_point>* ends; const std::vector<int>* dur;
    GW(size_t n,std::vector<Clock::time_point>* e,const std::vector<int>* d):ends(e),dur(d){
      for(size_t i=0;i<n;i++) ts.emplace_back([&]{
        while(true){size_t x;
          {std::unique_lock<std::mutex>lk(m);cv.wait(lk,[&]{return !running||!q.empty();});
           if(q.empty()){if(!running)break;else continue;}x=q.front();q.pop_front();}
          spin_us((*dur)[x]);(*ends)[x]=Clock::now();done++;
        }
      });
    }
    void submit(size_t x){{std::lock_guard<std::mutex>lk(m);q.push_back(x);}cv.notify_one();}
    void wait(uint64_t n){while(done.load()<n)std::this_thread::yield();}
    void stop(){running=false;cv.notify_all();for(auto&t:ts)if(t.joinable())t.join();}
};

struct W{LockFreeLinkedListQueue<size_t> q; std::atomic<uint64_t> queued{0},avg{100000}; std::atomic<bool> busy{false};};

struct LSched{
    std::vector<std::unique_ptr<W>> ws; std::vector<std::thread> ts;
    std::atomic<bool> running{true}; std::atomic<uint64_t> done{0},steals{0},ctr{1};
    bool adaptive,do_steal,newpolicy; std::vector<Clock::time_point>* ends; const std::vector<int>* dur;
    LSched(size_t n,bool a,bool s,bool np,std::vector<Clock::time_point>*e,const std::vector<int>*d)
      :adaptive(a),do_steal(s),newpolicy(np),ends(e),dur(d){
      for(size_t i=0;i<n;i++)ws.push_back(std::make_unique<W>());
      for(size_t id=0;id<n;id++)ts.emplace_back([this,id]{
        auto& self=*ws[id];
        while(running.load()||self.queued.load()){
          size_t x; bool got=false;
          if(self.q.pop(x)){self.queued--;got=true;}
          else if(do_steal){
            if(newpolicy){
              uint64_t seed=ctr.fetch_add(0xD1B54A32D192ED03ULL);
              for(int a=0;a<2&&!got;a++){
                size_t v=((seed>>(a*24))^(seed*(a+1)))%ws.size();if(v==id)v=(v+1)%ws.size();
                if(ws[v]->queued.load()&&ws[v]->q.pop(x)){ws[v]->queued--;steals++;got=true;}
              }
            }else{
              size_t victim=ws.size();uint64_t vl=0;
              for(size_t j=0;j<ws.size();j++)if(j!=id){
                uint64_t q=ws[j]->queued.load(),b=ws[j]->busy.load()?1:0,l=(q+b)*ws[j]->avg.load();
                if(q&&l>vl){vl=l;victim=j;}
              }
              if(victim<ws.size()&&ws[victim]->q.pop(x)){ws[victim]->queued--;steals++;got=true;}
            }
          }
          if(!got){std::this_thread::yield();continue;}
          self.busy=true;auto st=Clock::now();spin_us((*dur)[x]);
          auto ns=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-st).count();
          uint64_t old=self.avg.load();while(!self.avg.compare_exchange_weak(old,(old*4+ns)/5)){}
          (*ends)[x]=Clock::now();self.busy=false;done++;
        }
      });
    }
    size_t choose(size_t seq){
      if(!adaptive)return seq%ws.size();
      auto load=[&](size_t i){return (ws[i]->queued.load()+(ws[i]->busy.load()?1ULL:0ULL))*ws[i]->avg.load();};
      if(newpolicy){
        uint64_t x=ctr.fetch_add(0x9E3779B97F4A7C15ULL);
        size_t a=x%ws.size(),b=((x>>32)^(x*0xBF58476D1CE4E5B9ULL))%ws.size();if(a==b)b=(b+1)%ws.size();
        return load(a)<=load(b)?a:b;
      }
      size_t best=0;uint64_t bl=~0ULL;for(size_t i=0;i<ws.size();i++){auto l=load(i);if(l<bl){bl=l;best=i;}}return best;
    }
    void submit(size_t x){auto i=choose(x);ws[i]->queued++;ws[i]->q.push(x);}
    void wait(uint64_t n){while(done.load()<n)std::this_thread::yield();}
    void stop(){running=false;for(auto&t:ts)if(t.joinable())t.join();}
};

template<class S>
Res run(S& s,const std::vector<int>& d,std::vector<Clock::time_point>& ends,size_t submitters){
  auto st=Clock::now();std::vector<std::thread> ps;size_t n=d.size(),base=n/submitters,rem=n%submitters,off=0;
  for(size_t p=0;p<submitters;p++){size_t cnt=base+(p<rem);size_t start=off;off+=cnt;ps.emplace_back([&,start,cnt]{for(size_t i=0;i<cnt;i++)s.submit(start+i);});}
  for(auto&t:ps)t.join();s.wait(n);auto en=Clock::now();s.stop();
  std::vector<double> lat;for(auto t:ends)lat.push_back(std::chrono::duration<double,std::milli>(t-st).count());
  double ms=std::chrono::duration<double,std::milli>(en-st).count();
  uint64_t ss=0;if constexpr(requires{s.steals.load();})ss=s.steals.load();
  return{ms,n*1000.0/ms,pct(lat,.5),pct(lat,.95),pct(lat,.99),ss};
}

int main(int argc,char**argv){
 if(argc<5)return 2;std::string kind=argv[1];size_t workers=std::stoull(argv[2]),submitters=std::stoull(argv[3]),n=std::stoull(argv[4]);
 std::vector<int>d(n);
 if(kind=="long"){std::mt19937 g(42);std::uniform_int_distribution<int>x(1,100);for(auto&u:d){int p=x(g);u=p<=90?100:p<=99?1000:10000;}}
 else {int us=std::stoi(argv[5]);std::fill(d.begin(),d.end(),us);}
 for(std::string mode:{"global","rr","old_adapt","old_steal","new_adapt","new_steal"}){
   std::vector<Clock::time_point> ends(n);Res r;
   if(mode=="global"){GW s(workers,&ends,&d);r=run(s,d,ends,submitters);}
   else {bool a=mode!="rr";bool st=mode.find("steal")!=std::string::npos;bool np=mode.find("new_")==0;
         LSched s(workers,a,st,np,&ends,&d);r=run(s,d,ends,submitters);}
   std::cout<<mode<<" "<<r.ms<<" "<<r.tps<<" "<<r.p50<<" "<<r.p95<<" "<<r.p99<<" "<<r.steals<<"\n";
 }
}
