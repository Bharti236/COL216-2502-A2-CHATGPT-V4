# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# Core simulator sources (exclude the provided FILE and the repository's main.cpp).
SIM_SRC = $(filter-out main.cpp,$(wildcard *.cpp))

# ==========================================
# make compile FILE=<filename.cpp>
# ==========================================
compile:
	@echo "Compiling simulator:"
	$(CXX) $(CXXFLAGS) $(FILE) $(SIM_SRC) -o main
	@echo "Build successful, 'main' created."

# ==========================================
# make run FILE=<filename.s>
# ==========================================
run:
	@echo "Preprocessing $(FILE)..."
	python3 compiler.py $(FILE)
	@echo "Preprocessing complete."
