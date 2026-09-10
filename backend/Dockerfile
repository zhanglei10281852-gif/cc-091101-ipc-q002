# 多阶段构建 - 支持 ARM 和 X86 架构
FROM --platform=$BUILDPLATFORM gcc:12 AS builder

WORKDIR /app

# 安装 CMake
RUN apt-get update && apt-get install -y cmake && rm -rf /var/lib/apt/lists/*

# 复制源代码
COPY CMakeLists.txt .
COPY src/ src/
COPY tests/ tests/

# 编译主程序和测试
RUN mkdir -p build && cd build && \
    cmake -DBUILD_TESTS=ON .. && \
    make -j$(nproc)

# 运行阶段
FROM --platform=$TARGETPLATFORM debian:bookworm-slim

WORKDIR /app

# 安装运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# 从构建阶段复制可执行文件
COPY --from=builder /app/build/ipc_demo /app/ipc_demo
COPY --from=builder /app/build/tests/ipc_tests /app/ipc_tests

# 设置执行权限
RUN chmod +x /app/ipc_demo /app/ipc_tests

# 创建启动脚本
RUN echo '#!/bin/bash\n\
case "$1" in\n\
  test)\n\
    ./ipc_tests\n\
    ;;\n\
  demo)\n\
    ./ipc_demo --all\n\
    ;;\n\
  interactive)\n\
    ./ipc_demo\n\
    ;;\n\
  *)\n\
    echo "Usage: docker run <image> [test|demo|interactive]"\n\
    echo "  test        - Run all test cases"\n\
    echo "  demo        - Run all IPC demonstrations"\n\
    echo "  interactive - Interactive menu mode"\n\
    echo ""\n\
    echo "Running demo by default..."\n\
    ./ipc_demo --all\n\
    ;;\n\
esac' > /app/entrypoint.sh && chmod +x /app/entrypoint.sh

ENTRYPOINT ["/bin/bash", "/app/entrypoint.sh"]
CMD ["demo"]
