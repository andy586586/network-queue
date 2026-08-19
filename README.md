# Low-Latency Adaptive Task Pipeline

A C++20 concurrency project connecting a bounded ingress ring to a runtime-aware worker scheduler:

```text
input -> SPSC circular buffer -> dispatcher -> adaptive scheduler
                                             -> worker-local lock-free queues
                                             -> worker threads
```

## Design

- `CircularBuffer<T>` is a fixed-capacity SPSC queue. Release/acquire publication lets one input thread and one dispatcher exchange tasks without a mutex.
- `LockFreeLinkedListQueue<T>` retains the original Michael-Scott dummy-node and helping algorithm. Two hazard pointers protect the head and successor during `pop`; removed nodes are retired and reclaimed in batches.
- `AdaptiveScheduler` uses power-of-two choices rather than scanning every worker. Its load estimate combines queued and currently executing work with an EWMA runtime estimate.
- An idle worker probes at most two candidate queues. This opportunistic stealing repairs residual imbalance without a continuous all-worker scan.

The worker queues remain general MPMC queues because dispatch and stealing can access them concurrently. The ingress ring deliberately has the narrower SPSC contract to keep that hot path bounded and allocation-free.

## Build and validate

```sh
make
make test
make sanitize
```

`make test` runs the 20,000-task end-to-end exactly-once check.

## Benchmarks

```sh
make benchmark
```

The benchmark includes two workload families:

- heterogeneous tasks: 90% 100 us, 9% 1 ms, 1% 10 ms
- short tasks: 1 us, 5 us, and 10 us across multiple worker and submitter counts

Results depend heavily on hardware, compiler, thread placement, and system load. Run the benchmark on the target machine rather than treating one recorded run as a universal performance claim.
