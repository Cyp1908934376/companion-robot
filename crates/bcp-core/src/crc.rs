//! CRC-16/CCITT checksum (polynomial 0x1021).

/// CRC-16/CCITT lookup table (polynomial 0x1021, initial value 0xFFFF).
static CRC_TABLE: [u16; 256] = {
    let mut table = [0u16; 256];
    let mut i = 0;
    while i < 256 {
        let mut crc = (i as u16) << 8;
        let mut j = 0;
        while j < 8 {
            if (crc & 0x8000) != 0 {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
            j += 1;
        }
        table[i] = crc;
        i += 1;
    }
    table
};

/// Compute CRC-16/CCITT over `data`.
///
/// Uses polynomial `0x1021`, initial value `0xFFFF`. The result is
/// not complemented, so it can be appended directly to the frame
/// and checked by computing CRC over the entire frame (including the
/// appended CRC bytes), which should yield zero.
pub fn crc16_ccitt(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &byte in data {
        let idx = ((crc >> 8) as u8 ^ byte) as usize;
        crc = (crc << 8) ^ CRC_TABLE[idx];
    }
    crc
}

/// Compute CRC-16/CCITT with an initial seed value.
pub fn crc16_ccitt_with_seed(data: &[u8], seed: u16) -> u16 {
    let mut crc = seed;
    for &byte in data {
        let idx = ((crc >> 8) as u8 ^ byte) as usize;
        crc = (crc << 8) ^ CRC_TABLE[idx];
    }
    crc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_crc_known_value() {
        // "123456789" is the standard CRC-16/CCITT check string
        let data = b"123456789";
        let crc = crc16_ccitt(data);
        // Standard CRC-16/CCITT (Kermit) result for "123456789" is 0x2189
        // But our variant (0xFFFF init, no complement) gives 0x29B1
        assert_eq!(crc, 0x29B1);
    }

    #[test]
    fn test_crc_empty() {
        assert_eq!(crc16_ccitt(b""), 0xFFFF);
    }

    #[test]
    fn test_crc_corruption_detection() {
        // CRC should detect any single-byte corruption
        let data = b"hello world";
        let crc = crc16_ccitt(data);
        let mut corrupted = data.to_vec();
        corrupted[0] ^= 0x01;
        let crc2 = crc16_ccitt(&corrupted);
        assert_ne!(crc, crc2);
    }

    #[test]
    fn test_crc_deterministic() {
        let data = [0xCB, 0x01, 0x10, 0x00, 0x42, 0x00, 0x02, 0x00];
        let crc1 = crc16_ccitt(&data);
        let crc2 = crc16_ccitt(&data);
        assert_eq!(crc1, crc2);
    }
}
