// Engine registration: the Hub learns an engine's identity by running
// `ArcaneEditor.exe --print-engine-info` and parsing one line of JSON.
//
// This exists so the Hub NEVER hardcodes a plugin ABI. A .arcproj requires
// engine.abi, so a Hub that guessed it would mint stale-ABI projects the moment
// the engine bumps, and those crash on open. Probe, then stamp what the engine
// reported.
//
// The parsing half is pure and tested here. The spawning half lives in lib.rs.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const EDITOR_EXE: &str = "ArcaneEditor.exe";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct EngineInfo {
    // No serde(default): a missing ABI must be a hard error, never a defaulted
    // 0. This number is stamped into every project the Hub creates.
    pub engine_abi: u32,
    pub build: String,
    #[serde(default)]
    pub exe_path: String,
}

// Accept either the exe itself or a directory containing it, because the user
// picks with a folder dialog but may also paste a full path.
pub fn resolve_editor_exe(p: &Path) -> PathBuf {
    let looks_like_exe = p
        .extension()
        .map(|e| e.eq_ignore_ascii_case("exe"))
        .unwrap_or(false);
    if looks_like_exe {
        p.to_path_buf()
    } else {
        p.join(EDITOR_EXE)
    }
}

// The probe prints ONE line. Tolerate surrounding whitespace and a trailing
// newline; reject anything that is not a complete JSON object with an ABI.
pub fn parse_probe_output(s: &str) -> Result<EngineInfo, String> {
    let trimmed = s.trim();
    if trimmed.is_empty() {
        return Err("engine printed nothing -- is this an Arcane engine?".to_string());
    }
    serde_json::from_str::<EngineInfo>(trimmed)
        .map_err(|e| format!("could not read the engine's identity: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    // A real payload, copied verbatim from a slice-1 run of
    // `ArcaneEditor.exe --print-engine-info`.
    const REAL: &str =
        r#"{"build":"Arcane 0.1 (M5) [Debug]","engineAbi":7,"exePath":"D:/x/ArcaneEditor.exe"}"#;

    #[test]
    fn parses_a_real_probe_payload() {
        let info = parse_probe_output(REAL).expect("should parse");
        assert_eq!(info.engine_abi, 7);
        assert_eq!(info.build, "Arcane 0.1 (M5) [Debug]");
        assert_eq!(info.exe_path, "D:/x/ArcaneEditor.exe");
    }

    #[test]
    fn tolerates_surrounding_whitespace_and_a_trailing_newline() {
        let info = parse_probe_output(&format!("  {REAL}\r\n")).expect("should parse");
        assert_eq!(info.engine_abi, 7);
    }

    #[test]
    fn rejects_non_json_rather_than_panicking() {
        assert!(parse_probe_output("not json at all").is_err());
        assert!(parse_probe_output("").is_err());
        assert!(parse_probe_output("   \n").is_err());
    }

    #[test]
    fn rejects_a_payload_missing_engine_abi() {
        // An older engine, or the wrong exe entirely. Must be a clean error --
        // a defaulted 0 here would mint broken projects.
        let r = parse_probe_output(r#"{"build":"x","exePath":"y"}"#);
        assert!(r.is_err(), "missing engineAbi must not silently default");
    }

    #[test]
    fn rejects_a_non_numeric_engine_abi() {
        assert!(parse_probe_output(r#"{"engineAbi":"seven","build":"b"}"#).is_err());
    }

    #[test]
    fn ignores_unknown_future_fields() {
        // Forward-compat: a newer engine may add keys, which must not break an
        // older Hub.
        let info = parse_probe_output(r#"{"engineAbi":9,"build":"b","exePath":"p","futureThing":true}"#)
            .expect("unknown fields must be ignored");
        assert_eq!(info.engine_abi, 9);
    }

    #[test]
    fn resolve_editor_exe_accepts_a_directory_or_the_exe_itself() {
        assert!(resolve_editor_exe(Path::new("C:/eng")).ends_with(EDITOR_EXE));
        assert!(resolve_editor_exe(Path::new("C:/eng/ArcaneEditor.exe")).ends_with(EDITOR_EXE));
        // A directory must not gain a second exe segment.
        assert_eq!(
            resolve_editor_exe(Path::new("C:/eng/ArcaneEditor.exe")),
            PathBuf::from("C:/eng/ArcaneEditor.exe")
        );
    }

    #[test]
    fn resolve_editor_exe_is_case_insensitive_about_the_extension() {
        assert_eq!(
            resolve_editor_exe(Path::new("C:/eng/ArcaneEditor.EXE")),
            PathBuf::from("C:/eng/ArcaneEditor.EXE")
        );
    }
}
