#!/usr/bin/env bash
# Builds the server and the test binary with plain g++ (no CMake needed).
set -e
mkdir -p data
g++ -std=c++17 -O2 -pthread -o url_shortener_server main.cpp
g++ -std=c++17 -O2 -pthread -o run_tests tests.cpp
echo "Built ./url_shortener_server and ./run_tests"
