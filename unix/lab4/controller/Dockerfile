FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ libhiredis-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY main.cpp .
RUN g++ -O2 -std=c++17 main.cpp -lhiredis -o controller
ENTRYPOINT ["/app/controller"]