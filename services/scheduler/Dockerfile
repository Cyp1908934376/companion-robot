FROM rust:1.78-slim-bookworm AS builder

RUN apt-get update && apt-get install -y pkg-config libssl-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY crates/bcp-core crates/bcp-core
COPY services/scheduler services/scheduler

RUN cargo build --release --manifest-path services/scheduler/Cargo.toml

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/services/scheduler/target/release/scheduler /usr/local/bin/scheduler

EXPOSE 8082 9182
ENTRYPOINT ["/usr/local/bin/scheduler"]
