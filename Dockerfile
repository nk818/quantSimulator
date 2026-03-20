FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-all-dev \
    libgtest-dev \
    liburing-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN g++ -O3 -std=c++20 -march=native -fopenmp main.cpp Simulator.cpp -luring -o qengine && \
    g++ -O3 -std=c++20 -march=native -fopenmp qengine_test.cpp Simulator.cpp -luring -o qengine_test
CMD ["./qengine"]
