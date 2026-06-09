FROM rust:1.78-slim-bookworm AS builder

RUN apt-get update && apt-get install -y pkg-config libssl-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY services/ai services/ai

RUN cargo build --release --manifest-path services/ai/Cargo.toml

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/services/ai/target/release/ai-service /usr/local/bin/ai-service

EXPOSE 8083 9183
ENTRYPOINT ["/usr/local/bin/ai-service"]
