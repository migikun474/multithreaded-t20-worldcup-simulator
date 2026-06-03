
#  Makefile — T20 World Cup Cricket Simulator
#  Course: Operating Systems (CSC-204)

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS  := -lpthread

TARGET   := t20_simulator
BUILD    := build

# Source files
SRCS := \
    src/main.cpp \
    src/core/match_state.cpp \
    src/core/event_queue.cpp \
    src/core/pitch_monitor.cpp \
    src/core/scoreboard.cpp \
    src/threads/bowler_thread.cpp \
    src/threads/batsman_thread.cpp \
    src/threads/fielder_thread.cpp \
    src/threads/umpire_thread.cpp \
    src/threads/scheduler_thread.cpp \
    src/threads/logger_thread.cpp \
    src/threads/commentator_thread.cpp \
    src/models/player.cpp \
    src/models/ball.cpp \
    src/models/probability_model.cpp \
    src/utils/random.cpp \
    src/utils/time_utils.cpp

OBJS := $(patsubst src/%.cpp, $(BUILD)/%.o, $(SRCS))

#  Default target 
.PHONY: all clean run run-sjf run-priority analysis help

all: $(BUILD) $(TARGET)

$(BUILD):
	@mkdir -p $(BUILD)/core $(BUILD)/threads $(BUILD)/scheduler \
	           $(BUILD)/models $(BUILD)/utils logs docs

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "  Build complete → ./$(TARGET)"
	@echo "  Usage:"
	@echo "    ./$(TARGET)              # Run both SJF and FCFS one after the other."
	@echo "    ./$(TARGET) --sjf        # Shortest Job First"
	@echo "    ./$(TARGET) --deadlock-demo   # Run a simulation with a very high probablity to demonstrate the working of deadlock detection "
	@echo ""

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Iinclude -c $< -o $@

# Run targets 
run: all
	./$(TARGET) --fcfs

run-sjf: all
	./$(TARGET) --sjf

# Analysis (requires Python + matplotlib + pandas) 
analysis:
	@echo "[analysis] Generating Gantt charts (FCFS + SJF)..."
	python3 analysis/gantt_plot.py --csv-fcfs logs/events_fcfs.csv --csv-sjf logs/events_sjf.csv --out-prefix docs/gantt
	@echo "[analysis] Generating stats report..."
	python3 analysis/stats_analysis.py --csv-fcfs logs/events_fcfs.csv --csv-sjf logs/events_sjf.csv --out docs/analysis.png
	@echo "[analysis] Done. See docs/gantt_fcfs.png, docs/gantt_sjf.png and docs/analysis.png"

# Clean 
clean:
	rm -rf $(BUILD) $(TARGET) logs/*.txt logs/*.csv docs/*.png docs/*.json
# Help 
help:
	@echo "Targets:"
	@echo "  make              — build the simulator"
	@echo "  make run-priority — build + run with Priority scheduler"
	@echo "  make analysis     — generate Gantt chart + stats plots"
	@echo "  make clean        — remove build artefacts"
