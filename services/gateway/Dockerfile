FROM rust:1.78-slim-bookworm AS builder

RUN apt-get update && apt-get install -y pkg-config libssl-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY crates/bcp-core crates/bcp-core
COPY services/gateway services/gateway

RUN cargo build --release --manifest-path services/gateway/Cargo.toml

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/services/gateway/target/release/gateway /usr/local/bin/gateway

EXPOSE 8080 9000 9180
ENTRYPOINT ["/usr/local/bin/gateway"]
