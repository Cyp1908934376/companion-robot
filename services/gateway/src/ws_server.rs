//! WebSocket server for robot connections.
//!
//! Each robot connection gets its own tokio task. The pipeline:
//! 1. Accept TCP connection
//! 2. Authenticate robot (challenge-response)
//! 3. Upgrade to WebSocket
//! 4. Bidirectional relay: WS ↔ NATS

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;
use tokio_tungstenite::accept_async;
use tokio_tungstenite::tungstenite::Message;
use futures_util::{SinkExt, StreamExt};
use governor::{
    clock::DefaultClock,
    state::InMemoryState,
    state::NotKeyed,
    RateLimiter,
};

use bcp_core::{BcpCodec, BcpFrame};
use bcp_core::constants::MAX_FRAME_LEN;

use crate::auth::RobotKeyStore;
use crate::conn_pool::SharedConnPool;
use crate::config::Config;
use crate::error::Result;
use crate::metrics;
use crate::nats_bridge::NatsBridge;
use crate::rate_limiter::{self, RateLimit};

/// Run the WebSocket server loop.
pub async fn run(config: Config, key_store: Arc<RobotKeyStore>) -> anyhow::Result<()> {
    let listener = TcpListener::bind(&config.ws_addr).await?;
    tracing::info!(addr = %config.ws_addr, "WebSocket server listening");

    let conn_pool = Arc::new(SharedConnPool::new(config.max_connections));
    let nats = NatsBridge::connect(&config.nats_url).await?;
    let nats = Arc::new(nats);

    let global_limiter = rate_limiter::new_global_limiter(config.global_rate);
    let auth_timeout = Duration::from_secs(config.auth_timeout_secs);

    // Start broadcast listener
    start_broadcast_listener(nats.clone(), conn_pool.clone()).await;

    loop {
        let (stream, addr) = listener.accept().await?;
        let pool = conn_pool.clone();
        let nats = nats.clone();
        let global_lim = global_limiter.clone();
        let key_store = key_store.clone();

        metrics::CONNECTION_ATTEMPTS.inc();

        tokio::spawn(async move {
            if let Err(e) = handle_connection(stream, addr, pool, nats, global_lim, auth_timeout, key_store.as_ref()).await {
                tracing::warn!(%addr, error = %e, "connection error");
            }
        });
    }
}

/// Handle a single robot connection.
async fn handle_connection(
    mut stream: TcpStream,
    addr: SocketAddr,
    conn_pool: Arc<SharedConnPool>,
    nats: Arc<NatsBridge>,
    global_limiter: Arc<RateLimiter<NotKeyed, InMemoryState, DefaultClock>>,
    auth_timeout: Duration,
    key_store: &RobotKeyStore,
) -> Result<()> {
    // ── Phase 1: Authentication ──
    let auth_result = crate::auth::authenticate(&mut stream, auth_timeout, key_store).await?;

    // ── Phase 2: WebSocket upgrade ──
    let ws_stream = accept_async(stream).await?;
    let (mut ws_sink, mut ws_stream) = ws_stream.split();

    // ── Phase 3: Register in connection pool ──
    let (cmd_tx, mut cmd_rx) = mpsc::channel::<BcpFrame>(64);
    let short_id = conn_pool
        .register(
            auth_result.machine_id,
            addr,
            auth_result.capabilities,
            auth_result.firmware_version,
            cmd_tx.clone(),
        )
        .await?;

    tracing::info!(short_id, %addr, "robot connected");

    let rate_limiter = RateLimit::new(
        rate_limiter::per_conn_quota(100),
        global_limiter,
    );

    // ── Phase 4: Subscribe to NATS downlink for this robot ──
    let _nats_rx = nats.subscribe_robot(short_id).await?;

    // ── Phase 5: Send registration acknowledgment ──
    let mut reg_ack = BcpFrame::new(0);
    reg_ack.push(bcp_core::Command::RegAck {
        short_id,
        heartbeat_interval: 5000,
    }).ok();
    send_frame(&mut ws_sink, &reg_ack).await?;

    // ── Phase 6: Bidirectional relay ──
    let start = Instant::now();

    loop {
        tokio::select! {
            // Uplink: WebSocket → NATS
            ws_msg = ws_stream.next() => {
                match ws_msg {
                    Some(Ok(Message::Binary(data))) => {
                        // Rate limit check
                        if rate_limiter.check().is_err() {
                            metrics::RATE_LIMITED.inc();
                            continue;
                        }

                        // Decode BCP frame from binary payload
                        match BcpCodec::decode(&data) {
                            Ok((frame, _)) => {
                                let latency = start.elapsed().as_secs_f64() * 1000.0;
                                metrics::record_latency_ms(latency);
                                metrics::MESSAGES_UPLINK.inc();

                                if let Err(e) = nats.publish_frame(short_id, &frame).await {
                                    metrics::NATS_ERRORS.inc();
                                    tracing::warn!(short_id, error = %e, "NATS publish failed");
                                }
                            }
                            Err(e) => {
                                tracing::warn!(short_id, error = %e, "BCP decode failed");
                            }
                        }
                    }
                    Some(Ok(Message::Ping(p))) => {
                        let _ = ws_sink.send(Message::Pong(p)).await;
                    }
                    Some(Ok(Message::Close(_))) | None => {
                        tracing::info!(short_id, "WebSocket closed");
                        break;
                    }
                    Some(Err(e)) => {
                        tracing::warn!(short_id, error = %e, "WebSocket error");
                        break;
                    }
                    _ => {} // ignore text messages, pongs, etc.
                }
            }

            // Downlink: NATS → WebSocket
            nats_msg = cmd_rx.recv() => {
                match nats_msg {
                    Some(frame) => {
                        metrics::MESSAGES_DOWNLINK.inc();
                        if let Err(e) = send_frame(&mut ws_sink, &frame).await {
                            tracing::warn!(short_id, error = %e, "WebSocket send failed");
                            break;
                        }
                    }
                    None => {
                        tracing::debug!(short_id, "NATS command channel closed");
                    }
                }
            }
        }
    }

    // ── Cleanup ──
    metrics::DISCONNECTS.inc();
    conn_pool.unregister(short_id).await;
    tracing::info!(short_id, "robot disconnected");

    Ok(())
}

/// Encode a BCP frame and send it as a binary WebSocket message.
async fn send_frame(
    ws: &mut (impl SinkExt<Message, Error = tokio_tungstenite::tungstenite::Error> + Unpin),
    frame: &BcpFrame,
) -> Result<()> {
    let mut buf = [0u8; MAX_FRAME_LEN];
    let len = BcpCodec::encode(frame, &mut buf)?;
    ws.send(Message::Binary(buf[..len].to_vec())).await?;
    Ok(())
}

/// Listen for broadcast emergency commands and forward to all connected robots.
async fn start_broadcast_listener(
    nats: Arc<NatsBridge>,
    conn_pool: Arc<SharedConnPool>,
) {
    tokio::spawn(async move {
        let mut broadcast_rx = match nats.subscribe_broadcast().await {
            Ok(rx) => rx,
            Err(e) => {
                tracing::error!(error = %e, "failed to subscribe to broadcast");
                return;
            }
        };

        loop {
            match broadcast_rx.recv().await {
                Some(frame) => {
                    let short_ids = conn_pool.short_ids().await;
                    for id in short_ids {
                        if let Some(sender) = conn_pool.sender(id).await {
                            let _ = sender.send(frame.clone()).await;
                        }
                    }
                    metrics::MESSAGES_DOWNLINK.inc();
                }
                None => break,
            }
        }
    });
}
