CXX      ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g
LDFLAGS  := -pthread
SRCS     := LockFreeStack.cpp main.cpp
HDRS     := LockFreeStack.hpp

.PHONY: all run asan tsan test soak clean

# Pass arguments through: make run ARGS="5 30"  (scale 5, 30 s soak)
ARGS ?=

all: stack

# Optimized build
stack: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -O2 $(SRCS) -o $@ $(LDFLAGS)

# AddressSanitizer + UndefinedBehaviorSanitizer
stack_asan: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -O1 -fno-omit-frame-pointer -fsanitize=address,undefined $(SRCS) -o $@ $(LDFLAGS)

# ThreadSanitizer (cannot be combined with ASan; ignores standalone fences)
stack_tsan: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=thread $(SRCS) -o $@ $(LDFLAGS)

run: stack
	./stack $(ARGS)

asan: stack_asan
	./stack_asan $(ARGS)

tsan: stack_tsan
	./stack_tsan $(ARGS)

test: run asan tsan

# Long ASan run: 10x iterations, 60 s soak
soak: stack_asan
	./stack_asan 10 60

clean:
	rm -f stack stack_asan stack_tsan