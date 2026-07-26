// .arcproj generation.
//
// ProjectManifest::FromJson REQUIRES formatVersion > 0, name, and engine.abi.
// Everything else is optional, so a content-only project simply omits
// gameModule -- EditorApp already supports the plugins-only case ("an empty
// gameModule makes a plugins-only host").
//
// The ABI is always PROBED and passed in, never a constant here. That is the
// whole reason the engine grew --print-engine-info.

use serde_json::json;

pub const FORMAT_VERSION: i64 = 1;

pub fn manifest_json(name: &str, engine_abi: u32) -> Result<String, String> {
    // serde_json, not string concatenation -- a project name with a quote in it
    // must not produce a corrupt manifest.
    let doc = json!({
        "formatVersion": FORMAT_VERSION,
        "name": name,
        "description": "",
        "engine": { "abi": engine_abi }
    });
    serde_json::to_string_pretty(&doc).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_has_the_three_required_fields() {
        let v: serde_json::Value = serde_json::from_str(&manifest_json("MyGame", 7).unwrap()).unwrap();
        assert!(v["formatVersion"].as_i64().unwrap() > 0);
        assert_eq!(v["name"], "MyGame");
        assert_eq!(v["engine"]["abi"], 7);
    }

    #[test]
    fn manifest_stamps_the_probed_abi_not_a_constant() {
        // The whole reason slice 1's probe exists. A hardcoded ABI mints
        // projects that crash on open the moment the engine bumps.
        let v: serde_json::Value = serde_json::from_str(&manifest_json("G", 42).unwrap()).unwrap();
        assert_eq!(v["engine"]["abi"], 42);
    }

    #[test]
    fn manifest_omits_game_module_for_a_content_only_project() {
        let v: serde_json::Value = serde_json::from_str(&manifest_json("G", 7).unwrap()).unwrap();
        assert!(v.get("gameModule").is_none());
    }

    #[test]
    fn manifest_escapes_names_safely() {
        let text = manifest_json("My \"Game\"", 7).unwrap();
        let v: serde_json::Value = serde_json::from_str(&text).expect("must stay valid JSON");
        assert_eq!(v["name"], "My \"Game\"");
    }

    #[test]
    fn manifest_survives_a_backslash_in_the_name() {
        let text = manifest_json("A\\B", 7).unwrap();
        let v: serde_json::Value = serde_json::from_str(&text).expect("must stay valid JSON");
        assert_eq!(v["name"], "A\\B");
    }
}
