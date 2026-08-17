
#include "lf_queue.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
struct Work { int us; };
struct Result { double elapsed_ms, throughput, p50, p95, p99; uint64_t steals=0; };

static double pct(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    size_t i = std::min(v.size()-1, (size_t)(q*(v.size()-1)));
    return v[i];
}
static Result summarize(Clock::time_point start, Clock::time_point end,
                        const std::vector<Clock::time_point>& done, uint64_t steals=0) {
    std::vector<double> lat;
    lat.reserve(done.size());
    for (auto t: done)
        lat.push_back(std::chrono::duration<double,std::milli>(t-start).count());
    double ms=std::chrono::duration<double,std::milli>(end-start).count();
    return {ms, done.size()*1000.0/ms, pct(lat,.50), pct(lat,.95), pct(lat,.99), steals};
}
static void doWork(int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

Result globalQ(const std::vector<Work>& jobs, size_t n) {
    std::deque<size_t> q; std::mutex m; std::condition_variable cv;
    bool done_submit=false; std::vector<Clock::time_point> done(jobs.size());
    std::vector<std::thread> ts;
    auto start=Clock::now();
    for(size_t w=0;w<n;w++) ts.emplace_back([&]{
        while(true) {
            size_t i;
            { std::unique_lock<std::mutex> lk(m); cv.wait(lk,[&]{return done_submit||!q.empty();});
              if(q.empty()){ if(done_submit) break; else continue; }
              i=q.front(); q.pop_front(); }
            doWork(jobs[i].us); done[i]=Clock::now();
        }
    });
    { std::lock_guard<std::mutex> lk(m); for(size_t i=0;i<jobs.size();i++) q.push_back(i); done_submit=true; }
    cv.notify_all(); for(auto& t:ts)t.join();
    return summarize(start,Clock::now(),done);
}

struct LFW {
    LockFreeLinkedListQueue<size_t> q;
    std::atomic<size_t> queued{0};
    std::atomic<uint64_t> avg{100000};
    std::atomic<bool> busy{false};
};

Result localLFQ(const std::vector<Work>& jobs, size_t n, bool adaptive, bool stealing) {
    std::vector<std::unique_ptr<LFW>> ws; for(size_t i=0;i<n;i++)ws.push_back(std::make_unique<LFW>());
    std::vector<Clock::time_point> done(jobs.size());
    std::atomic<bool> submitted{false}; std::atomic<uint64_t> steals{0};
    auto choose=[&](size_t seq){
        if(!adaptive) return seq%n;
        size_t best=0; uint64_t bl=std::numeric_limits<uint64_t>::max();
        for(size_t i=0;i<n;i++){
            uint64_t q=ws[i]->queued.load(std::memory_order_relaxed);
            uint64_t b=ws[i]->busy.load(std::memory_order_relaxed)?1:0;
            uint64_t l=(q+b)*ws[i]->avg.load(std::memory_order_relaxed);
            if(l<bl){bl=l;best=i;}
        } return best;
    };
    auto start=Clock::now(); std::vector<std::thread> ts;
    for(size_t id=0;id<n;id++) ts.emplace_back([&,id]{
        while(!submitted.load(std::memory_order_acquire)||ws[id]->queued.load()>0) {
            size_t idx; bool got=false;
            if(ws[id]->q.pop(idx)){ws[id]->queued.fetch_sub(1);got=true;}
            else if(stealing){
                size_t victim=n; uint64_t vl=0;
                for(size_t j=0;j<n;j++) if(j!=id){
                    uint64_t q=ws[j]->queued.load(); uint64_t b=ws[j]->busy.load()?1:0;
                    uint64_t l=(q+b)*ws[j]->avg.load();
                    if(q>0&&l>vl){vl=l;victim=j;}
                }
                if(victim<n && ws[victim]->q.pop(idx)){
                    ws[victim]->queued.fetch_sub(1); steals.fetch_add(1); got=true;
                }
            }
            if(!got){std::this_thread::yield();continue;}
            ws[id]->busy.store(true);
            auto st=Clock::now(); doWork(jobs[idx].us);
            auto ns=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-st).count();
            uint64_t old=ws[id]->avg.load();
            while(!ws[id]->avg.compare_exchange_weak(old,(old*4+ns)/5)){}
            done[idx]=Clock::now(); ws[id]->busy.store(false);
        }
    });
    for(size_t i=0;i<jobs.size();i++){size_t w=choose(i);ws[w]->queued.fetch_add(1);ws[w]->q.push(i);}
    submitted.store(true,std::memory_order_release);
    for(auto& t:ts)t.join();
    return summarize(start,Clock::now(),done,steals.load());
}

int main(){
    constexpr size_t N=5000,W=4,TRIALS=5;
    std::vector<std::vector<Work>> all;
    for(size_t tr=0;tr<TRIALS;tr++){
        std::mt19937 rng(42+tr); std::uniform_int_distribution<int>d(1,100);
        std::vector<Work> v; v.reserve(N);
        for(size_t i=0;i<N;i++){int p=d(rng);v.push_back({p<=90?100:(p<=99?1000:10000)});}
        all.push_back(std::move(v));
    }
    const char* names[]={"global_mutex","round_robin_lfq","adaptive_busy_lfq","adaptive_busy_steal"};
    std::vector<std::vector<Result>> rs(4);
    for(size_t tr=0;tr<TRIALS;tr++){
        rs[0].push_back(globalQ(all[tr],W));
        rs[1].push_back(localLFQ(all[tr],W,false,false));
        rs[2].push_back(localLFQ(all[tr],W,true,false));
        rs[3].push_back(localLFQ(all[tr],W,true,true));
    }
    for(int k=0;k<4;k++){
        auto med=[&](auto f){std::vector<double>x;for(auto&r:rs[k])x.push_back(f(r));std::sort(x.begin(),x.end());return x[x.size()/2];};
        std::cout<<names[k]
          <<" elapsed_ms="<<med([](auto&r){return r.elapsed_ms;})
          <<" throughput="<<med([](auto&r){return r.throughput;})
          <<" p50_ms="<<med([](auto&r){return r.p50;})
          <<" p95_ms="<<med([](auto&r){return r.p95;})
          <<" p99_ms="<<med([](auto&r){return r.p99;})
          <<" steals="<<med([](auto&r){return (double)r.steals;})<<"\n";
    }
}
