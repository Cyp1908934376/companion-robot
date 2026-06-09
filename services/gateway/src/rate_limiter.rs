//! Rate limiting for gateway connections.
//!
//! Implements a token-bucket rate limiter per connection and globally.

use governor::{
    clock::DefaultClock,
    state::{InMemoryState, NotKeyed},
    Quota, RateLimiter,
};
use nonzero_ext::nonzero;
use std::num::NonZeroU32;
use std::sync::Arc;

use crate::error::{GatewayError, Result};

/// Per-connection and global rate limiter.
pub struct RateLimit {
    /// Per-connection limiter (token bucket).
    per_conn: RateLimiter<NotKeyed, InMemoryState, DefaultClock>,
    /// Global limiter shared across all connections.
    global: Arc<RateLimiter<NotKeyed, InMemoryState, DefaultClock>>,
}

impl RateLimit {
    /// Create a new per-connection rate limiter with shared global limiter.
    pub fn new(
        per_conn_rate: NonZeroU32,
        global: Arc<RateLimiter<NotKeyed, InMemoryState, DefaultClock>>,
    ) -> Self {
        RateLimit {
            per_conn: RateLimiter::direct(Quota::per_second(per_conn_rate)),
            global,
        }
    }

    /// Check both per-connection and global rate limits.
    /// Returns `Err(RateLimited)` if either limit is exceeded.
    pub fn check(&self) -> Result<()> {
        if self.per_conn.check().is_err() {
            return Err(GatewayError::RateLimited);
        }
        if self.global.check().is_err() {
            return Err(GatewayError::RateLimited);
        }
        Ok(())
    }
}

/// Create a global rate limiter shared across all connections.
pub fn new_global_limiter(rate: u32) -> Arc<RateLimiter<NotKeyed, InMemoryState, DefaultClock>> {
    let quota = Quota::per_second(NonZeroU32::new(rate.max(1)).unwrap_or(nonzero!(1u32)));
    Arc::new(RateLimiter::direct(quota))
}

/// Per-connection rate (default 100 msg/s).
pub fn per_conn_quota(rate: u32) -> NonZeroU32 {
    NonZeroU32::new(rate.max(1)).unwrap_or(nonzero!(100u32))
}
