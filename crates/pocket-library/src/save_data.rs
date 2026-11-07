use std::fs;
use std::io::{self, ErrorKind};
use std::path::{Path, PathBuf};

pub const SAVE_DIR_NAME: &str = "saves";
pub const SAVE_FILE_NAME: &str = "save.dat";
const MAGIC: &[u8; 8] = b"PHSAVE01";
const VERSION: u32 = 1;
const MAX_PAYLOAD: usize = 64 * 1024 * 1024;

pub fn save_dir_for(library_root: &Path, game_id: &str) -> PathBuf {
    library_root.join("games").join(game_id).join(SAVE_DIR_NAME)
}

pub fn save_path_for(library_root: &Path, game_id: &str) -> PathBuf {
    save_dir_for(library_root, game_id).join(SAVE_FILE_NAME)
}

pub fn save_bytes(path: &Path, payload: &[u8]) -> io::Result<()> {
    if payload.len() > MAX_PAYLOAD {
        return Err(io::Error::new(
            ErrorKind::InvalidInput,
            "save payload is too large",
        ));
    }
    let parent = path
        .parent()
        .ok_or_else(|| io::Error::new(ErrorKind::InvalidInput, "save path has no parent"))?;
    fs::create_dir_all(parent)?;
    let mut bytes = Vec::with_capacity(MAGIC.len() + 8 + payload.len() + 4);
    bytes.extend_from_slice(MAGIC);
    bytes.extend_from_slice(&VERSION.to_le_bytes());
    bytes.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    bytes.extend_from_slice(payload);
    let checksum = crc32(payload);
    bytes.extend_from_slice(&checksum.to_le_bytes());
    let temporary = path.with_extension("tmp");
    fs::write(&temporary, bytes)?;
    fs::rename(temporary, path)
}

pub fn load_bytes(path: &Path) -> io::Result<Option<Vec<u8>>> {
    let bytes = match fs::read(path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error),
    };
    if bytes.len() < MAGIC.len() + 8 + 4 || &bytes[..MAGIC.len()] != MAGIC {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            "invalid save header",
        ));
    }
    let mut at = MAGIC.len();
    let version = read_u32(&bytes, &mut at)?;
    if version != VERSION {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            "unsupported save version",
        ));
    }
    let length = read_u64(&bytes, &mut at)? as usize;
    if length > MAX_PAYLOAD
        || at
            .checked_add(length + 4)
            .filter(|end| *end == bytes.len())
            .is_none()
    {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            "invalid save length",
        ));
    }
    let payload = bytes[at..at + length].to_vec();
    at += length;
    let expected = read_u32(&bytes, &mut at)?;
    if crc32(&payload) != expected {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            "save checksum mismatch",
        ));
    }
    Ok(Some(payload))
}

fn read_u32(bytes: &[u8], at: &mut usize) -> io::Result<u32> {
    let end = at
        .checked_add(4)
        .ok_or_else(|| io::Error::new(ErrorKind::InvalidData, "truncated save"))?;
    let value = bytes
        .get(*at..end)
        .ok_or_else(|| io::Error::new(ErrorKind::InvalidData, "truncated save"))?;
    *at = end;
    Ok(u32::from_le_bytes(value.try_into().unwrap()))
}

fn read_u64(bytes: &[u8], at: &mut usize) -> io::Result<u64> {
    let end = at
        .checked_add(8)
        .ok_or_else(|| io::Error::new(ErrorKind::InvalidData, "truncated save"))?;
    let value = bytes
        .get(*at..end)
        .ok_or_else(|| io::Error::new(ErrorKind::InvalidData, "truncated save"))?;
    *at = end;
    Ok(u64::from_le_bytes(value.try_into().unwrap()))
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0xffff_ffff;
    for byte in bytes {
        crc ^= *byte as u32;
        for _ in 0..8 {
            let mask = 0u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0xedb8_8320 & mask);
        }
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_save_is_a_clean_new_game() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(load_bytes(&dir.path().join("missing.dat")).unwrap(), None);
    }

    #[test]
    fn save_round_trip_and_corruption_are_detected() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("saves").join("save.dat");
        save_bytes(&path, b"level=4;score=9001").unwrap();
        assert_eq!(load_bytes(&path).unwrap().unwrap(), b"level=4;score=9001");
        let mut bytes = fs::read(&path).unwrap();
        let last = bytes.len() - 1;
        bytes[last] ^= 1;
        fs::write(&path, bytes).unwrap();
        assert_eq!(
            load_bytes(&path).unwrap_err().kind(),
            ErrorKind::InvalidData
        );
    }
}
