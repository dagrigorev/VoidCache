# Используем официальный образ Ubuntu с поддержкой C++20
FROM ubuntu:22.04

# Устанавливаем зависимости
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    gdb \
    git \
    ninja-build \
    clang-14 \
    lldb-14 \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# Настраиваем clang как компилятор по умолчанию
RUN update-alternatives --install /usr/bin/cc cc /usr/bin/clang-14 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-14 100

RUN mkdir /workspace

# Рабочая директория
WORKDIR /workspace