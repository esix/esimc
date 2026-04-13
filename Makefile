BISON    := /opt/homebrew/opt/bison/bin/bison
FLEX     := flex
CXX      := /opt/homebrew/opt/llvm/bin/clang++
CC       := clang

LLVM_CXXFLAGS := $(shell llvm-config --cxxflags)
LLVM_LDFLAGS  := $(shell llvm-config --ldflags --libs core)

CXXFLAGS := -std=c++17 -Wall $(LLVM_CXXFLAGS)
LDFLAGS  := $(LLVM_LDFLAGS) -lz -lcurses

BUILD   := build
SRCDIR  := src
RUNTIME := runtime

# Generated sources
PARSER_CPP := $(BUILD)/parser.tab.cpp
PARSER_H   := $(BUILD)/parser.tab.hpp
LEXER_CPP  := $(BUILD)/lexer.cpp

OBJS := $(BUILD)/codegen.o $(BUILD)/main.o $(BUILD)/parser.tab.o $(BUILD)/lexer.o

.PHONY: all clean runtime

all: $(BUILD)/esimc $(BUILD)/simula_rt.o

$(BUILD):
	mkdir -p $(BUILD)

# ---- Compiler ----

$(PARSER_CPP) $(PARSER_H): $(SRCDIR)/parser.y | $(BUILD)
	$(BISON) -d --defines=$(PARSER_H) -o $(PARSER_CPP) $<

$(LEXER_CPP): $(SRCDIR)/lexer.l $(PARSER_H) | $(BUILD)
	$(FLEX) -o $@ $<

$(BUILD)/parser.tab.o: $(PARSER_CPP) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -I$(BUILD) -c -o $@ $<

$(BUILD)/lexer.o: $(LEXER_CPP) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Wno-deprecated-register -I$(SRCDIR) -I$(BUILD) -c -o $@ $<

$(BUILD)/codegen.o: $(SRCDIR)/codegen.cpp $(SRCDIR)/codegen.h $(SRCDIR)/ast.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -I$(BUILD) -c -o $@ $<

$(BUILD)/main.o: $(SRCDIR)/main.cpp $(SRCDIR)/ast.h $(SRCDIR)/codegen.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -I$(BUILD) -c -o $@ $<

$(BUILD)/esimc: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

# ---- Runtime library ----

$(BUILD)/simula_rt.o: $(RUNTIME)/simula_rt.c $(RUNTIME)/simula_rt.h | $(BUILD)
	$(CC) -Wall -O2 -c -o $@ $<

runtime: $(BUILD)/simula_rt.o

# ---- Convenience: compile and run a .sim file ----
# Usage: make run SIM=examples/hello.sim

run: $(BUILD)/esimc $(BUILD)/simula_rt.o
	@$(BUILD)/esimc $(SIM) -o $(BUILD)/output.ll
	@llc -filetype=obj $(BUILD)/output.ll -o $(BUILD)/output.o
	@clang $(BUILD)/output.o $(BUILD)/simula_rt.o -o $(BUILD)/a.out
	@$(BUILD)/a.out

clean:
	rm -rf $(BUILD)
