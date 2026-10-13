//! minisign signing (spec s5). Keys are always encrypted, and always opened
//! with a password: minisign's `into_secret_key(None)` prompts on the
//! terminal, which would hang CI.

use std::io::Cursor;

use minisign::{KeyPair, SecretKey, SecretKeyBox};

#[derive(Debug, thiserror::Error)]
pub enum SignError {
    #[error("the secret key could not be opened (wrong password, or not a minisign secret key): {0}")]
    Open(String),
    #[error("an empty password is not allowed")]
    EmptyPassword,
    #[error("signing failed: {0}")]
    Sign(String),
}

pub struct GeneratedKey {
    /// The .pub file's text: an untrusted comment line, then the key in base64.
    pub public_key_file: String,
    /// The .key file's text (encrypted).
    pub secret_key_file: String,
    pub public_key_base64: String,
}

pub fn generate(comment: &str, password: &str) -> Result<GeneratedKey, SignError> {
    if password.is_empty() {
        return Err(SignError::EmptyPassword);
    }
    let (mut public, mut secret) = (Vec::new(), Vec::new());
    let pair = KeyPair::generate_and_write_encrypted_keypair(&mut public, &mut secret, Some(comment), Some(password.to_string()))
        .map_err(|e| SignError::Sign(e.to_string()))?;
    Ok(GeneratedKey {
        public_key_file: String::from_utf8(public).expect("minisign writes UTF-8"),
        secret_key_file: String::from_utf8(secret).expect("minisign writes UTF-8"),
        public_key_base64: pair.pk.to_base64(),
    })
}

pub fn open_secret_key(secret_key_file: &str, password: &str) -> Result<SecretKey, SignError> {
    if password.is_empty() {
        return Err(SignError::EmptyPassword);
    }
    SecretKeyBox::from_string(secret_key_file)
        .and_then(|b| b.into_secret_key(Some(password.to_string())))
        .map_err(|e| SignError::Open(e.to_string()))
}

/// Signs `bytes` and returns the .minisig file's text. The trusted comment is
/// covered by the signature.
pub fn sign(sk: &SecretKey, bytes: &[u8], trusted_comment: &str) -> Result<String, SignError> {
    minisign::sign(None, sk, Cursor::new(bytes), Some(trusted_comment), Some("signature from the Arcane release key"))
        .map(|b| b.into_string())
        .map_err(|e| SignError::Sign(e.to_string()))
}
