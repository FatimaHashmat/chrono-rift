CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt

BIN_DIR = build
TARGETS = $(BIN_DIR)/arbiter $(BIN_DIR)/hip $(BIN_DIR)/asp

all: $(BIN_DIR) $(TARGETS)
	@echo Build complete.

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/arbiter: arbiter/arbiter.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

$(BIN_DIR)/hip: hip/hip.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)

$(BIN_DIR)/asp: asp/asp.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -rf $(BIN_DIR)

.PHONY: all clean
