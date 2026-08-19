CXX ?= c++
CPPFLAGS := -Isrc
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra -Wpedantic -pthread
PROGRAMS := test_pipeline benchmark compare_bench contention_bench
.PHONY: all test sanitize benchmark clean
all: $(addprefix build/,$(PROGRAMS))
build/test_pipeline: tests/test_pipeline.cpp src/adaptive_scheduler.hpp src/circular_buffer.hpp src/lf_queue.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@
build/%: benchmarks/%.cpp src/adaptive_scheduler.hpp src/lf_queue.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@
test: build/test_pipeline
	./build/test_pipeline
sanitize:
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined tests/test_pipeline.cpp -o build/test_pipeline_san
	./build/test_pipeline_san
benchmark: build/benchmark build/compare_bench build/contention_bench
	./build/benchmark
	./build/compare_bench long 4 1 5000
	./build/compare_bench short 8 4 50000 1
	./build/contention_bench global 8 4 50000 5
	./build/contention_bench steal 8 4 50000 5
clean:
	rm -rf build
