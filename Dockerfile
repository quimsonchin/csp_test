FROM mcr.microsoft.com/devcontainers/base:debian-12 
RUN apt-get update && apt-get install -y ninja-build cmake wget unzip gcc g++
RUN wget "https://github.com/libcsp/libcsp/archive/refs/tags/v2.1.zip" -O libcsp.zip
RUN unzip libcsp.zip && \
    cd libcsp-2.1 && \
    cmake -B build -G Ninja && \
    cmake --build build && \
    cmake --install build --prefix /usr
    