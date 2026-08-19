
#include "lf_queue.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static inline void spin_for_us(int us) {
    if (us <= 0) return;
    auto end = Clock::now() + std::chrono::microseconds(us);
    while (Clock::now() < end) {
        asm volatile("" ::: "memory");
    }
}

struct Result {
    double elapsed_ms;
    double throughput;
    uint64_t steals;
};

struct GlobalMutexScheduler {
    std::deque<int> q;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::thread> workers;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> done{0};
    int task_us;

    GlobalMutexScheduler(size_t n, int us):task_us(us){
        for(size_t i=0;i<n;i++){
            workers.emplace_back([this]{
                while(true){
                    int x;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        cv.wait(lk,[&]{ return !running.load() || !q.empty(); });
                        if(q.empty()){
                            if(!running.load()) break;
                            continue;
                        }
                        x=q.front(); q.pop_front();
                    }
                    spin_for_us(task_us);
                    done.fetch_add(1,std::memory_order_relaxed);
                }
            });
        }
    }
    void submit(int x){
        { std::lock_guard<std::mutex> lk(m); q.push_back(x); }
        cv.notify_one();
    }
    void wait(uint64_t total){
        while(done.load(std::memory_order_acquire)<total) std::this_thread::yield();
    }
    void stop(){
        running.store(false); cv.notify_all();
        for(auto& t:workers) if(t.joinable()) t.join();
    }
    ~GlobalMutexScheduler(){ stop(); }
};

struct LFWorker {
    LockFreeLinkedListQueue<int> q;
    std::atomic<uint64_t> queued{0};
    std::atomic<uint64_t> avg_ns{1000};
    std::atomic<bool> busy{false};
};

struct LocalLFQScheduler {
    std::vector<std::unique_ptr<LFWorker>> ws;
    std::vector<std::thread> workers;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> steals{0};
    std::atomic<uint64_t> rr{0};
    int task_us;
    bool adaptive;
    bool stealing;

    LocalLFQScheduler(size_t n,int us,bool a,bool s):task_us(us),adaptive(a),stealing(s){
        for(size_t i=0;i<n;i++) ws.push_back(std::make_unique<LFWorker>());
        for(size_t id=0;id<n;id++){
            workers.emplace_back([this,id]{
                auto& self=*ws[id];
                while(running.load(std::memory_order_acquire) || self.queued.load()>0){
                    int x; bool got=false;
                    if(self.q.pop(x)){
                        self.queued.fetch_sub(1); got=true;
                    } else if(stealing){
                        size_t victim=ws.size(); uint64_t vl=0;
                        for(size_t j=0;j<ws.size();j++) if(j!=id){
                            uint64_t q=ws[j]->queued.load(std::memory_order_relaxed);
                            uint64_t b=ws[j]->busy.load(std::memory_order_relaxed)?1:0;
                            uint64_t l=(q+b)*ws[j]->avg_ns.load(std::memory_order_relaxed);
                            if(q>0 && l>vl){vl=l;victim=j;}
                        }
                        if(victim<ws.size() && ws[victim]->q.pop(x)){
                            ws[victim]->queued.fetch_sub(1);
                            steals.fetch_add(1);
                            got=true;
                        }
                    }
                    if(!got){ std::this_thread::yield(); continue; }
                    self.busy.store(true,std::memory_order_relaxed);
                    auto st=Clock::now();
                    spin_for_us(task_us);
                    auto ns=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-st).count();
                    uint64_t old=self.avg_ns.load(std::memory_order_relaxed);
                    while(!self.avg_ns.compare_exchange_weak(old,(old*7+ns)/8,std::memory_order_relaxed)){}
                    self.busy.store(false,std::memory_order_relaxed);
                    done.fetch_add(1,std::memory_order_relaxed);
                }
            });
        }
    }

    size_t choose(){
        if(!adaptive) return rr.fetch_add(1,std::memory_order_relaxed)%ws.size();
        size_t best=0; uint64_t bl=std::numeric_limits<uint64_t>::max();
        for(size_t i=0;i<ws.size();i++){
            uint64_t q=ws[i]->queued.load(std::memory_order_relaxed);
            uint64_t b=ws[i]->busy.load(std::memory_order_relaxed)?1:0;
            uint64_t l=(q+b)*ws[i]->avg_ns.load(std::memory_order_relaxed);
            if(l<bl){bl=l;best=i;}
        }
        return best;
    }

    void submit(int x){
        size_t i=choose();
        ws[i]->queued.fetch_add(1,std::memory_order_relaxed);
        ws[i]->q.push(x);
    }
    void wait(uint64_t total){
        while(done.load(std::memory_order_acquire)<total) std::this_thread::yield();
    }
    void stop(){
        running.store(false);
        for(auto& t:workers) if(t.joinable()) t.join();
    }
    ~LocalLFQScheduler(){ stop(); }
};

template<class Scheduler>
Result run_case(size_t workers,size_t submitters,uint64_t total,int task_us, Scheduler&& sched){
    auto start=Clock::now();
    std::vector<std::thread> producers;
    uint64_t base=total/submitters, rem=total%submitters;
    for(size_t s=0;s<submitters;s++){
        uint64_t cnt=base+(s<rem?1:0);
        producers.emplace_back([&sched,cnt,s]{
            for(uint64_t i=0;i<cnt;i++) sched.submit((int)(i+s));
        });
    }
    for(auto& t:producers)t.join();
    sched.wait(total);
    auto end=Clock::now();
    uint64_t steals=0;
    if constexpr (requires { sched.steals.load(); }) steals=sched.steals.load();
    sched.stop();
    double ms=std::chrono::duration<double,std::milli>(end-start).count();
    return {ms,total*1000.0/ms,steals};
}

int main(int argc,char**argv){
    if(argc!=6){std::cerr<<"usage mode workers submitters total task_us\n";return 2;}
    std::string mode=argv[1];
    size_t workers=std::stoull(argv[2]), submitters=std::stoull(argv[3]);
    uint64_t total=std::stoull(argv[4]); int us=std::stoi(argv[5]);
    Result r;
    if(mode=="global"){
        GlobalMutexScheduler s(workers,us);
        r=run_case(workers,submitters,total,us,std::move(s));
    } else if(mode=="rr"){
        LocalLFQScheduler s(workers,us,false,false);
        r=run_case(workers,submitters,total,us,std::move(s));
    } else if(mode=="adaptive"){
        LocalLFQScheduler s(workers,us,true,false);
        r=run_case(workers,submitters,total,us,std::move(s));
    } else if(mode=="steal"){
        LocalLFQScheduler s(workers,us,true,true);
        r=run_case(workers,submitters,total,us,std::move(s));
    } else return 3;
    std::cout<<r.elapsed_ms<<" "<<r.throughput<<" "<<r.steals<<"\n";
}
