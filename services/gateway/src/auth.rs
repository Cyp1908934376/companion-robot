//! Machine authentication via Ed25519 challenge-response.
//!
//! Flow:
//! 1. Gateway sends 32-byte random challenge
//! 2. Robot signs challenge with its Ed25519 private key
//! 3. Robot responds with: signature(64) + public_key(32) + capabilities(2) + fw_ver(4)
//! 4. Gateway verifies signature against the provided public key
//! 5. Gateway derives machine_id = SHA-256(public_key)[..16]
//! 6. Gateway checks key store for authorization
//!
//! In dev mode (no key store), any robot with a valid signature is auto-authorized.

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use rand::RngCore;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::path::Path;
use std::sync::RwLock;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::timeout;

use crate::error::{GatewayError, Result};

/// 128-bit machine identifier (derived from public key hash).
pub type MachineId = [u8; 16];

/// Auth response length: signature(64) + public_key(32) + capabilities(2) + fw_ver(4)
const AUTH_RESPONSE_LEN: usize = 102;

/// Result of successful authentication.
#[derive(Debug, Clone)]
#[allow(dead_code)]
pub struct AuthResult {
    pub machine_id: MachineId,
    pub public_key: [u8; 32],
    pub capabilities: u16,
    pub firmware_version: [u8; 4],
}

// ── Robot Key Store ────────────────────────────────────────────

/// Authorized robot entry in the key store.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct AuthorizedRobot {
    /// Human-readable name
    pub name: String,
    /// Hex-encoded Ed25519 public key (64 hex chars)
    pub public_key_hex: String,
    /// Machine ID derived from the key (32 hex chars)
    pub machine_id_hex: String,
    /// Authorization timestamp (RFC 3339)
    pub authorized_at: String,
}

/// Manages authorized robot public keys.
///
/// Loaded from a JSON file. In dev mode (no file), auto-authorizes
/// any robot presenting a valid Ed25519 signature.
pub struct RobotKeyStore {
    /// Map from machine_id_hex → AuthorizedRobot
    authorized: RwLock<HashMap<String, AuthorizedRobot>>,
    /// Whether to auto-authorize unknown robots (dev mode)
    auto_authorize: bool,
    /// Path to the key store file for persistence
    store_path: Option<String>,
}

impl RobotKeyStore {
    /// Create a key store from a JSON file.
    pub fn load(path: &str) -> std::io::Result<Self> {
        let path = Path::new(path);
        let authorized: HashMap<String, AuthorizedRobot> = if path.exists() {
            let data = fs::read_to_string(path)?;
            let robots: Vec<AuthorizedRobot> =
                serde_json::from_str(&data).unwrap_or_default();
            robots
                .into_iter()
                .map(|r| (r.machine_id_hex.clone(), r))
                .collect()
        } else {
            tracing::warn!("key store file not found at {}, creating empty store", path.display());
            HashMap::new()
        };

        tracing::info!(count = authorized.len(), "loaded robot key store");
        Ok(Self {
            authorized: RwLock::new(authorized),
            auto_authorize: false,
            store_path: Some(path.to_string_lossy().to_string()),
        })
    }

    /// Create a dev-mode store that auto-authorizes any robot.
    pub fn dev_mode() -> Self {
        tracing::warn!("using dev-mode key store — all robots will be auto-authorized");
        Self {
            authorized: RwLock::new(HashMap::new()),
            auto_authorize: true,
            store_path: None,
        }
    }

    /// Check if a machine_id is authorized.
    pub fn is_authorized(&self, machine_id_hex: &str) -> bool {
        if self.auto_authorize {
            return true;
        }
        self.authorized.read().unwrap().contains_key(machine_id_hex)
    }

    /// Authorize a new robot and persist to the key store.
    pub fn authorize(
        &self,
        name: &str,
        public_key_hex: &str,
        machine_id_hex: &str,
    ) -> std::io::Result<()> {
        let entry = AuthorizedRobot {
            name: name.to_string(),
            public_key_hex: public_key_hex.to_string(),
            machine_id_hex: machine_id_hex.to_string(),
            authorized_at: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| d.as_secs().to_string())
                .unwrap_or_default(),
        };

        {
            let mut authorized = self.authorized.write().unwrap();
            authorized.insert(machine_id_hex.to_string(), entry.clone());
        }

        self.persist()?;

        tracing::info!(
            name = %name,
            machine_id = %machine_id_hex,
            "robot authorized"
        );
        Ok(())
    }

    /// Persist current authorized set to the JSON file.
    fn persist(&self) -> std::io::Result<()> {
        if let Some(ref path) = self.store_path {
            let authorized = self.authorized.read().unwrap();
            let entries: Vec<&AuthorizedRobot> = authorized.values().collect();
            let json = serde_json::to_string_pretty(&entries)?;
            fs::write(path, json)?;
        }
        Ok(())
    }

    /// Get the number of authorized robots.
    pub fn count(&self) -> usize {
        self.authorized.read().unwrap().len()
    }
}

// ── Auth Protocol ──────────────────────────────────────────────

/// Derive a machine ID from a public key: SHA-256(pk)[..16].
pub fn derive_machine_id(public_key: &[u8; 32]) -> MachineId {
    let mut hasher = Sha256::new();
    hasher.update(public_key);
    let hash = hasher.finalize();
    let mut id = [0u8; 16];
    id.copy_from_slice(&hash[..16]);
    id
}

/// Send challenge and verify response over a TCP stream.
pub async fn authenticate(
    stream: &mut TcpStream,
    auth_timeout: Duration,
    key_store: &RobotKeyStore,
) -> Result<AuthResult> {
    // 1. Generate and send 32-byte challenge
    let mut challenge = [0u8; 32];
    rand::thread_rng().fill_bytes(&mut challenge);

    let write_fut = stream.write_all(&challenge);
    timeout(auth_timeout, write_fut)
        .await
        .map_err(|_| GatewayError::AuthFailed("challenge write timeout".into()))?
        .map_err(|e| GatewayError::IoError(e))?;

    // 2. Read response: signature(64) + public_key(32) + capabilities(2) + fw_ver(4) = 102 bytes
    let mut response = [0u8; AUTH_RESPONSE_LEN];
    let read_fut = stream.read_exact(&mut response);
    timeout(auth_timeout, read_fut)
        .await
        .map_err(|_| GatewayError::AuthFailed("response read timeout".into()))?
        .map_err(|e| GatewayError::IoError(e))?;

    // 3. Parse response fields
    let mut sig_bytes = [0u8; 64];
    sig_bytes.copy_from_slice(&response[..64]);
    let signature = Signature::from_bytes(&sig_bytes);

    let mut public_key_bytes = [0u8; 32];
    public_key_bytes.copy_from_slice(&response[64..96]);
    let capabilities = u16::from_le_bytes([response[96], response[97]]);
    let mut firmware_version = [0u8; 4];
    firmware_version.copy_from_slice(&response[98..102]);

    // 4. Verify Ed25519 signature
    let verifying_key = VerifyingKey::from_bytes(&public_key_bytes)
        .map_err(|e| GatewayError::AuthFailed(format!("invalid public key: {}", e)))?;

    verifying_key
        .verify(&challenge, &signature)
        .map_err(|e| GatewayError::AuthFailed(format!("signature verification failed: {}", e)))?;

    // 5. Derive machine_id from public key
    let machine_id = derive_machine_id(&public_key_bytes);
    let machine_id_hex = hex::encode(machine_id);
    let pk_hex = hex::encode(public_key_bytes);

    // 6. Authorization check
    if !key_store.is_authorized(&machine_id_hex) {
        // Auto-authorize in dev mode, reject in production mode
        if key_store.auto_authorize {
            let _ = key_store.authorize(
                &format!("robot-{}", &machine_id_hex[..8]),
                &pk_hex,
                &machine_id_hex,
            );
        } else {
            return Err(GatewayError::AuthFailed(format!(
                "robot not authorized: {}",
                machine_id_hex
            )));
        }
    }

    tracing::info!(
        machine_id = %machine_id_hex,
        capabilities,
        fw_ver = ?firmware_version,
        "robot authenticated"
    );

    Ok(AuthResult {
        machine_id,
        public_key: public_key_bytes,
        capabilities,
        firmware_version,
    })
}

// ── Tests ──────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};
    use rand::rngs::OsRng;
    use rand::RngCore;

    fn random_signing_key() -> SigningKey {
        let mut seed = [0u8; 32];
        OsRng.fill_bytes(&mut seed);
        SigningKey::from_bytes(&seed)
    }

    #[test]
    fn test_derive_machine_id_deterministic() {
        let pk = [0xABu8; 32];
        let id1 = derive_machine_id(&pk);
        let id2 = derive_machine_id(&pk);
        assert_eq!(id1, id2);
    }

    #[test]
    fn test_derive_machine_id_different_keys() {
        let pk1 = [0x01u8; 32];
        let pk2 = [0x02u8; 32];
        assert_ne!(derive_machine_id(&pk1), derive_machine_id(&pk2));
    }

    #[test]
    fn test_derive_machine_id_length() {
        let pk = [0u8; 32];
        let id = derive_machine_id(&pk);
        assert_eq!(id.len(), 16);
    }

    #[test]
    fn test_sign_and_verify_roundtrip() {
        let signing_key = random_signing_key();
        let verifying_key = signing_key.verifying_key();
        let challenge = [0x42u8; 32];
        let signature = signing_key.sign(&challenge);
        assert!(verifying_key.verify(&challenge, &signature).is_ok());
    }

    #[test]
    fn test_bad_signature_rejected() {
        let signing_key = random_signing_key();
        let verifying_key = signing_key.verifying_key();
        let challenge = [0x42u8; 32];
        let wrong_challenge = [0x99u8; 32];
        let signature = signing_key.sign(&challenge);
        assert!(verifying_key.verify(&wrong_challenge, &signature).is_err());
    }

    #[test]
    fn test_wrong_key_rejected() {
        let signing_key1 = random_signing_key();
        let signing_key2 = random_signing_key();
        let challenge = [0x42u8; 32];
        let signature = signing_key1.sign(&challenge);
        assert!(signing_key2
            .verifying_key()
            .verify(&challenge, &signature)
            .is_err());
    }

    #[test]
    fn test_key_store_auto_authorize() {
        let store = RobotKeyStore::dev_mode();
        assert!(store.is_authorized("any_random_id"));
        assert!(store.auto_authorize);
    }

    #[test]
    fn test_key_store_reject_unknown_in_prod() {
        let signing_key = random_signing_key();
        let pk: [u8; 32] = signing_key.verifying_key().to_bytes();
        let mid = derive_machine_id(&pk);
        let mid_hex = hex::encode(mid);

        let store = RobotKeyStore {
            authorized: RwLock::new(HashMap::new()),
            auto_authorize: false,
            store_path: None,
        };

        assert!(!store.is_authorized(&mid_hex));
    }

    #[test]
    fn test_key_store_authorize_and_check() {
        let signing_key = random_signing_key();
        let pk_bytes: [u8; 32] = signing_key.verifying_key().to_bytes();
        let mid = derive_machine_id(&pk_bytes);

        let store = RobotKeyStore {
            authorized: RwLock::new(HashMap::new()),
            auto_authorize: false,
            store_path: None,
        };

        let mid_hex = hex::encode(mid);
        let pk_hex = hex::encode(pk_bytes);

        assert!(!store.is_authorized(&mid_hex));
        store.authorize("test-bot", &pk_hex, &mid_hex).unwrap();
        assert!(store.is_authorized(&mid_hex));
    }

    #[test]
    fn test_auth_response_format() {
        assert_eq!(AUTH_RESPONSE_LEN, 102);

        let signing_key = random_signing_key();
        let pk = signing_key.verifying_key().to_bytes();
        let challenge = [0x77u8; 32];
        let sig = signing_key.sign(&challenge);

        let mut response = [0u8; 102];
        response[..64].copy_from_slice(&sig.to_bytes());
        response[64..96].copy_from_slice(&pk);
        response[96] = 0x0F;
        response[97] = 0x00;
        response[98..102].copy_from_slice(&[1, 0, 0, 0]);

        let sig_bytes: [u8; 64] = response[..64].try_into().unwrap();
        let parsed_sig = Signature::from_bytes(&sig_bytes);
        let parsed_pk: [u8; 32] = response[64..96].try_into().unwrap();
        let caps = u16::from_le_bytes([response[96], response[97]]);
        let mut fw = [0u8; 4];
        fw.copy_from_slice(&response[98..102]);

        let vk = VerifyingKey::from_bytes(&parsed_pk).unwrap();
        assert!(vk.verify(&challenge, &parsed_sig).is_ok());
        assert_eq!(caps, 0x0F);
        assert_eq!(fw, [1, 0, 0, 0]);
    }
}
