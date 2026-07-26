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

/// The human name for a recorded project path.
///
/// Recents holds BOTH shapes: entries recorded before the Hub asked for a
/// `.arcproj` file are folders, and new ones are the manifest file itself.
/// Both must render the same name, so "D:/Games/MyGame" and
/// "D:/Games/MyGame/MyGame.arcproj" both yield "MyGame".
///
/// Pure string work on purpose -- it runs against paths that may no longer
/// exist on disk, so it must not consult the filesystem.
pub fn display_name(path: &str) -> String {
    let last = path
        .rsplit(['/', '\\'])
        .find(|s| !s.is_empty())
        .unwrap_or(path);

    // Case-insensitive: Windows does not care how the extension is spelled.
    let stem = match last.rfind('.') {
        Some(i) if last[i..].eq_ignore_ascii_case(".arcproj") => &last[..i],
        _ => last,
    };

    if stem.is_empty() {
        path.to_string()
    } else {
        stem.to_string()
    }
}

pub const MANIFEST_EXT: &str = "arcproj";

/// Pick the single manifest out of a folder's file names, or None.
///
/// Mirrors `Project::Open` (Project.cpp:29-41): a folder with two `.arcproj`
/// files is AMBIGUOUS and the engine refuses it, so the Hub must not guess
/// either. Pure so the rule is testable without a filesystem.
pub fn pick_manifest(names: &[String]) -> Option<&str> {
    let mut found = None;
    for n in names {
        let is_manifest = n
            .rsplit_once('.')
            .is_some_and(|(stem, ext)| !stem.is_empty() && ext.eq_ignore_ascii_case(MANIFEST_EXT));
        if is_manifest {
            if found.is_some() {
                return None; // ambiguous, same as the engine's own verdict
            }
            found = Some(n.as_str());
        }
    }
    found
}

/// Read `engine.abi` out of a `.arcproj` document.
///
/// This is the ABI the project was BUILT AGAINST, and it is the only number
/// that answers "will this engine open it" -- `Runtime::OpenProject`
/// (Runtime.cpp:387) compares exactly this field against the engine's own
/// constant and refuses on mismatch.
///
/// None on any malformed shape, which the caller treats as "unknown" rather
/// than as a conflict: we must not brand a project broken on a parse failure.
pub fn parse_manifest_abi(text: &str) -> Option<u32> {
    let doc: serde_json::Value = serde_json::from_str(text).ok()?;
    // u64 first, then narrow: a negative or fractional abi is malformed, not 0.
    let n = doc.get("engine")?.get("abi")?.as_u64()?;
    u32::try_from(n).ok()
}

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

    fn names(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn pick_manifest_finds_the_single_one() {
        let n = names(&["Content", "MyGame.arcproj", ".gitignore"]);
        assert_eq!(pick_manifest(&n), Some("MyGame.arcproj"));
    }

    #[test]
    fn pick_manifest_refuses_an_ambiguous_folder() {
        // Project.cpp:37 returns nullopt here; guessing would pick a different
        // project than the engine will.
        let n = names(&["A.arcproj", "B.arcproj"]);
        assert_eq!(pick_manifest(&n), None);
    }

    #[test]
    fn pick_manifest_is_case_insensitive_about_the_extension() {
        assert_eq!(pick_manifest(&names(&["G.ARCPROJ"])), Some("G.ARCPROJ"));
    }

    #[test]
    fn pick_manifest_returns_none_when_there_is_none() {
        assert_eq!(pick_manifest(&names(&["Content", "readme.md"])), None);
        assert_eq!(pick_manifest(&[]), None);
    }

    #[test]
    fn pick_manifest_ignores_a_bare_extension_with_no_stem() {
        // ".arcproj" is a hidden file, not a project named "".
        assert_eq!(pick_manifest(&names(&[".arcproj"])), None);
    }

    #[test]
    fn parse_manifest_abi_reads_the_built_against_abi() {
        let text = manifest_json("G", 5).unwrap();
        assert_eq!(parse_manifest_abi(&text), Some(5));
    }

    #[test]
    fn parse_manifest_abi_reads_a_manifest_the_engine_wrote() {
        // Project::Create emits gameModule/plugins/bootScene too; extra keys
        // must not matter.
        let text = r#"{"formatVersion":1,"name":"G","engine":{"abi":9},
                       "gameModule":"","plugins":[],"bootScene":""}"#;
        assert_eq!(parse_manifest_abi(text), Some(9));
    }

    #[test]
    fn parse_manifest_abi_is_none_for_malformed_shapes() {
        // None means "unknown", and the UI treats unknown as "cannot prove a
        // conflict" -- so every one of these must NOT come back as Some(0).
        assert_eq!(parse_manifest_abi("not json"), None);
        assert_eq!(parse_manifest_abi("{}"), None);
        assert_eq!(parse_manifest_abi(r#"{"engine":{}}"#), None);
        assert_eq!(parse_manifest_abi(r#"{"engine":7}"#), None);
        assert_eq!(parse_manifest_abi(r#"{"engine":{"abi":"7"}}"#), None);
        assert_eq!(parse_manifest_abi(r#"{"engine":{"abi":-1}}"#), None);
        assert_eq!(parse_manifest_abi(r#"{"engine":{"abi":1.5}}"#), None);
    }

    #[test]
    fn parse_manifest_abi_accepts_zero_as_a_stated_value() {
        // Distinct from None: the manifest said 0, which isCompatible treats as
        // "unknown" anyway, but the distinction must not be invented here.
        assert_eq!(parse_manifest_abi(r#"{"engine":{"abi":0}}"#), Some(0));
    }

    #[test]
    fn display_name_is_the_same_for_a_folder_and_its_manifest() {
        // The whole point: switching the Open dialog to pick the .arcproj must
        // not make the same project show up under a different name.
        assert_eq!(display_name("D:/Games/MyGame"), "MyGame");
        assert_eq!(display_name("D:/Games/MyGame/MyGame.arcproj"), "MyGame");
        assert_eq!(display_name("D:\\Games\\MyGame\\MyGame.arcproj"), "MyGame");
    }

    #[test]
    fn display_name_uses_the_manifest_stem_when_it_differs_from_the_folder() {
        // Project::Open takes whatever single .arcproj it finds, so the file
        // name is the authority when one was picked directly.
        assert_eq!(display_name("D:/Games/checkout/Aphelyon.arcproj"), "Aphelyon");
    }

    #[test]
    fn display_name_ignores_extension_case() {
        assert_eq!(display_name("D:/G/MyGame.ARCPROJ"), "MyGame");
    }

    #[test]
    fn display_name_keeps_dots_that_are_not_the_extension() {
        assert_eq!(display_name("D:/G/My.Game.arcproj"), "My.Game");
        assert_eq!(display_name("D:/G/My.Game"), "My.Game");
    }

    #[test]
    fn display_name_tolerates_a_trailing_separator() {
        assert_eq!(display_name("D:/Games/MyGame/"), "MyGame");
        assert_eq!(display_name("D:\\Games\\MyGame\\"), "MyGame");
    }

    #[test]
    fn display_name_falls_back_to_the_whole_path_when_there_is_no_name() {
        // Never return an empty card label; showing the raw path at least
        // tells the user which entry is broken.
        assert_eq!(display_name("/"), "/");
        assert_eq!(display_name(""), "");
        assert_eq!(display_name(".arcproj"), ".arcproj");
    }

    #[test]
    fn manifest_survives_a_backslash_in_the_name() {
        let text = manifest_json("A\\B", 7).unwrap();
        let v: serde_json::Value = serde_json::from_str(&text).expect("must stay valid JSON");
        assert_eq!(v["name"], "A\\B");
    }
}
