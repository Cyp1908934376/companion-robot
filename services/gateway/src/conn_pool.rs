//! Connection pool — tracks all active robot connections.
//!
//! Allocates short (16-bit) cluster IDs and maintains the mapping
//! between machine_id, short_id, and the WebSocket sender handle.

use std::collections::HashMap;
use std::net::SocketAddr;

use tokio::sync::{mpsc, RwLock};

use crate::error::{GatewayError, Result};

/// Information about a connected robot.
#[derive(Debug, Clone)]
#[allow(dead_code)]
pub struct ConnInfo {
    pub short_id: u16,
    pub machine_id: [u8; 16],
    pub addr: SocketAddr,
    pub capabilities: u16,
    pub firmware_version: [u8; 4],
    pub connected_at: std::time::Instant,
}

/// Sender handle for pushing BCP frames to a specific robot's WebSocket.
pub type RobotSender = mpsc::Sender<bcp_core::BcpFrame>;

/// Thread-safe connection pool.
pub struct ConnPool {
    /// Next short ID to allocate (wrapping 16-bit).
    next_id: u16,
    /// Active connections by short_id.
    by_short_id: HashMap<u16, ConnInfo>,
    /// Active connections by machine_id.
    by_machine_id: HashMap<[u8; 16], u16>,
    /// Sender channels by short_id (for pushing frames to robots).
    senders: HashMap<u16, RobotSender>,
    /// Maximum allowed connections.
    max_connections: usize,
}

impl ConnPool {
    pub fn new(max_connections: usize) -> Self {
        ConnPool {
            next_id: 1,
            by_short_id: HashMap::new(),
            by_machine_id: HashMap::new(),
            senders: HashMap::new(),
            max_connections,
        }
    }

    /// Register a newly authenticated robot.
    /// Returns the assigned short_id. If the machine_id already has an active
    /// connection, the old one is replaced.
    pub fn register(
        &mut self,
        machine_id: [u8; 16],
        addr: SocketAddr,
        capabilities: u16,
        firmware_version: [u8; 4],
        sender: RobotSender,
    ) -> Result<u16> {
        // If already connected, reuse the short_id (kick old connection)
        if let Some(&existing_id) = self.by_machine_id.get(&machine_id) {
            self.by_short_id.remove(&existing_id);
            self.senders.remove(&existing_id);
            tracing::warn!(short_id = existing_id, "replaced existing connection");
        }

        // Check connection limit
        if self.by_short_id.len() >= self.max_connections {
            return Err(GatewayError::ConnectionLimit(self.max_connections));
        }

        // Allocate a new short_id (skip 0, find unused)
        let short_id = self.allocate_id();

        let info = ConnInfo {
            short_id,
            machine_id,
            addr,
            capabilities,
            firmware_version,
            connected_at: std::time::Instant::now(),
        };

        self.by_short_id.insert(short_id, info);
        self.by_machine_id.insert(machine_id, short_id);
        self.senders.insert(short_id, sender);

        tracing::info!(
            short_id,
            addr = %addr,
            "robot registered"
        );

        Ok(short_id)
    }

    /// Unregister a disconnected robot.
    pub fn unregister(&mut self, short_id: u16) {
        if let Some(info) = self.by_short_id.remove(&short_id) {
            self.by_machine_id.remove(&info.machine_id);
            self.senders.remove(&short_id);
            tracing::info!(short_id, "robot unregistered");
        }
    }

    /// Look up connection info by short_id.
    #[allow(dead_code)]
    pub fn get(&self, short_id: u16) -> Option<&ConnInfo> {
        self.by_short_id.get(&short_id)
    }

    /// Get the sender handle for a robot.
    pub fn sender(&self, short_id: u16) -> Option<&RobotSender> {
        self.senders.get(&short_id)
    }

    /// Number of active connections.
    #[allow(dead_code)]
    pub fn len(&self) -> usize {
        self.by_short_id.len()
    }

    /// All active short_ids.
    pub fn short_ids(&self) -> Vec<u16> {
        self.by_short_id.keys().copied().collect()
    }

    /// Allocate the next available short_id.
    fn allocate_id(&mut self) -> u16 {
        let start = self.next_id;
        loop {
            let candidate = self.next_id;
            self.next_id = self.next_id.wrapping_add(1);
            if self.next_id == 0 {
                self.next_id = 1; // skip 0
            }
            if !self.by_short_id.contains_key(&candidate) {
                return candidate;
            }
            if self.next_id == start {
                // Wrapped around and all IDs taken
                panic!("all 65535 short_ids are in use");
            }
        }
    }
}

/// Thread-safe wrapper around ConnPool.
pub struct SharedConnPool {
    inner: RwLock<ConnPool>,
}

impl SharedConnPool {
    pub fn new(max_connections: usize) -> Self {
        SharedConnPool {
            inner: RwLock::new(ConnPool::new(max_connections)),
        }
    }

    pub async fn register(
        &self,
        machine_id: [u8; 16],
        addr: SocketAddr,
        capabilities: u16,
        firmware_version: [u8; 4],
        sender: RobotSender,
    ) -> Result<u16> {
        self.inner.write().await.register(machine_id, addr, capabilities, firmware_version, sender)
    }

    pub async fn unregister(&self, short_id: u16) {
        self.inner.write().await.unregister(short_id);
    }

    #[allow(dead_code)]
    pub async fn get(&self, short_id: u16) -> Option<ConnInfo> {
        self.inner.read().await.get(short_id).cloned()
    }

    pub async fn sender(&self, short_id: u16) -> Option<RobotSender> {
        self.inner.read().await.sender(short_id).cloned()
    }

    #[allow(dead_code)]
    pub async fn len(&self) -> usize {
        self.inner.read().await.len()
    }

    pub async fn short_ids(&self) -> Vec<u16> {
        self.inner.read().await.short_ids()
    }
}

// ── Tests ───────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn make_machine_id(v: u8) -> [u8; 16] {
        let mut id = [0u8; 16];
        id[0] = v;
        id
    }

    fn dummy_sender() -> RobotSender {
        let (tx, _rx) = mpsc::channel(8);
        tx
    }

    fn dummy_addr() -> SocketAddr {
        "127.0.0.1:9000".parse().unwrap()
    }

    #[test]
    fn test_register_and_lookup() {
        let mut pool = ConnPool::new(100);
        let machine_id = make_machine_id(1);
        let short_id = pool.register(machine_id, dummy_addr(), 0x000F, [0, 1, 0, 0], dummy_sender())
            .expect("register should succeed");

        let info = pool.get(short_id).expect("should find by short_id");
        assert_eq!(info.machine_id, machine_id);
        assert_eq!(info.short_id, short_id);
        assert_eq!(pool.len(), 1);
    }

    #[test]
    fn test_register_multiple() {
        let mut pool = ConnPool::new(100);

        let id1 = pool.register(make_machine_id(1), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();
        let id2 = pool.register(make_machine_id(2), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();

        assert_ne!(id1, id2);
        assert_eq!(pool.len(), 2);
        assert!(pool.get(id1).is_some());
        assert!(pool.get(id2).is_some());
    }

    #[test]
    fn test_unregister() {
        let mut pool = ConnPool::new(100);
        let machine_id = make_machine_id(1);
        let short_id = pool.register(machine_id, dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();

        pool.unregister(short_id);
        assert!(pool.get(short_id).is_none());
        assert_eq!(pool.len(), 0);
    }

    #[test]
    fn test_unregister_nonexistent() {
        let mut pool = ConnPool::new(100);
        // Should not panic
        pool.unregister(999);
    }

    #[test]
    fn test_duplicate_machine_id_replaces() {
        let mut pool = ConnPool::new(100);
        let machine_id = make_machine_id(1);

        let first_id = pool.register(machine_id, dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();
        let second_id = pool.register(machine_id, dummy_addr(), 1, [0; 4], dummy_sender()).unwrap();

        // Re-registration allocates a new ID (old is freed, new allocated)
        assert_eq!(pool.len(), 1);
        // First ID should now be available (pool has 1 entry with second_id)
        assert!(pool.get(first_id).is_none());
        assert!(pool.get(second_id).is_some());
    }

    #[test]
    fn test_connection_limit() {
        let mut pool = ConnPool::new(2);
        pool.register(make_machine_id(1), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();
        pool.register(make_machine_id(2), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();

        let result = pool.register(make_machine_id(3), dummy_addr(), 0, [0; 4], dummy_sender());
        assert!(result.is_err());
    }

    #[test]
    fn test_short_ids_list() {
        let mut pool = ConnPool::new(100);
        pool.register(make_machine_id(1), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();
        pool.register(make_machine_id(2), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();

        let mut ids = pool.short_ids();
        ids.sort();
        assert_eq!(ids.len(), 2);
    }

    #[test]
    fn test_sender_handle() {
        let mut pool = ConnPool::new(100);
        let (tx, _rx) = mpsc::channel(8);
        let machine_id = make_machine_id(1);
        let short_id = pool.register(machine_id, dummy_addr(), 0, [0; 4], tx.clone()).unwrap();

        let _retrieved = pool.sender(short_id).expect("should find sender");
        // Can't compare senders directly, but we can verify non-existent lookup
        assert!(pool.sender(999).is_none());
    }

    #[test]
    fn test_unregister_frees_slot() {
        let mut pool = ConnPool::new(100);
        let id1 = pool.register(make_machine_id(1), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();
        assert_eq!(pool.len(), 1);

        pool.unregister(id1);
        assert_eq!(pool.len(), 0);
        assert!(pool.get(id1).is_none());

        // Re-register a new machine — old ID slot is freed, count stays correct
        let id2 = pool.register(make_machine_id(2), dummy_addr(), 0, [0; 4], dummy_sender()).unwrap();
        assert_eq!(pool.len(), 1);
        assert!(pool.get(id2).is_some());
    }
}
