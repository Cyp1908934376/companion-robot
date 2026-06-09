FROM rust:1.78-slim-bookworm AS builder

RUN apt-get update && apt-get install -y pkg-config libssl-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY crates/bcp-core crates/bcp-core
COPY services/device-manager services/device-manager

RUN cargo build --release --manifest-path services/device-manager/Cargo.toml

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/services/device-manager/target/release/device-manager /usr/local/bin/device-manager

EXPOSE 8081 9181
ENTRYPOINT ["/usr/local/bin/device-manager"]
