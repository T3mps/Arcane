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

/// Split a typed argument string into argv tokens.
///
/// Whitespace separates; double quotes group a token containing spaces and are
/// stripped from the result. There is no escape character -- this is a
/// convenience field for `--backend vulkan` and `--scene "My Level"`, not a
/// shell, and inventing backslash escaping on Windows would mean a path
/// argument could not be pasted in as written.
///
/// Tokenising HERE and passing an argv array is the whole point: `Command`
/// passes each token to the child as-is, so nothing the user types can be
/// re-interpreted as shell syntax.
pub fn split_args(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quoted = false;
    let mut has = false; // distinguishes an empty quoted token from no token

    for c in s.chars() {
        match c {
            '"' => {
                quoted = !quoted;
                has = true;
            }
            c if c.is_whitespace() && !quoted => {
                if has {
                    out.push(std::mem::take(&mut cur));
                    has = false;
                }
            }
            c => {
                cur.push(c);
                has = true;
            }
        }
    }
    if has {
        out.push(cur);
    }
    out
}

// Characters Windows rejects in a file or folder name, plus the reserved DOS
// device names. Spaces and hyphens are deliberately absent -- "My Game" and
// "3d-demo" are ordinary folder names.
const ILLEGAL_NAME_CHARS: [char; 9] = ['<', '>', ':', '"', '/', '\\', '|', '?', '*'];
const RESERVED_STEMS: [&str; 22] = [
    "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8",
    "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
];
const MAX_NAME_LEN: usize = 64;

/// Why `name` is not usable as a project (and therefore folder) name, or None.
///
/// PAIRED with `projectNameError` in src/lib/format.ts, which runs the same
/// rules as you type. That one is for feedback; this one is the gate -- these
/// are IPC arguments, and `rename_project` turns them into a directory name and
/// a file name, so it cannot rely on the caller having checked.
pub fn name_error(name: &str) -> Option<String> {
    let n = name.trim();
    if n.is_empty() {
        return Some("Enter a project name.".into());
    }
    if let Some(c) = n.chars().find(|c| ILLEGAL_NAME_CHARS.contains(c)) {
        return Some(format!("A project name cannot contain \"{c}\"."));
    }
    if n.chars().any(|c| c.is_control()) {
        return Some("A project name cannot contain a control character.".into());
    }
    // Windows silently strips a trailing dot, so the folder on disk would not
    // match the name the user typed. Trailing SPACE is not checked because it
    // cannot reach here: `n` is trimmed above, and callers use the trimmed
    // value, so "MyGame " really does become the folder "MyGame".
    if n.ends_with('.') {
        return Some("A project name cannot end with a dot.".into());
    }
    let stem = n.split('.').next().unwrap_or(n).to_ascii_lowercase();
    if RESERVED_STEMS.contains(&stem.as_str()) {
        return Some(format!("\"{n}\" is a name Windows reserves."));
    }
    if n.chars().count() > MAX_NAME_LEN {
        return Some(format!(
            "Keep the name under {MAX_NAME_LEN} characters (this one is {}).",
            n.chars().count()
        ));
    }
    None
}

/// The .arcproj a shell invocation carried, if any: double-clicking an
/// associated file starts (or signals) the Hub with the file somewhere in
/// argv. First match wins; matching on the extension alone is safe because
/// argv[0] -- the exe path, included by both the cold-start args and the
/// single-instance forward -- can never end in .arcproj, so no skip(1)
/// guesswork about which shape arrived.
pub fn arcproj_in_args<'a>(args: impl IntoIterator<Item = &'a str>) -> Option<&'a str> {
    args.into_iter()
        .find(|a| a.to_ascii_lowercase().ends_with(".arcproj"))
}

/// Directory names Duplicate never copies, at ANY depth: Binaries and
/// Intermediate are build outputs the duplicate rebuilds from source (and
/// they appear under Plugins/* as well as at the root, hence any-depth), and
/// .git because a duplicate is a NEW project, not a second checkout quietly
/// pushing to the same remote.
pub const DUPLICATE_SKIP: [&str; 3] = ["Binaries", "Intermediate", ".git"];

/// Case-insensitive, because Windows paths are.
pub fn skip_in_duplicate(dir_name: &str) -> bool {
    DUPLICATE_SKIP.iter().any(|s| dir_name.eq_ignore_ascii_case(s))
}

/// The nth candidate name for a duplicate: "X Copy", then "X Copy 2", ...
/// Pure so the format is pinned by a test rather than by whatever the loop
/// in duplicate_project happens to do.
pub fn copy_name(base: &str, n: u32) -> String {
    if n <= 1 {
        format!("{base} Copy")
    } else {
        format!("{base} Copy {n}")
    }
}

/// Why `root` must not be handed to a recursive delete, or None if it is safe.
///
/// The caller has already established that `root` is a folder holding exactly
/// one `.arcproj` -- that is the real guard, and it is what makes "delete this
/// folder" a defined operation at all. These two are the belt: a relative path
/// would be resolved against whatever the Hub's working directory happens to
/// be, and a path with no parent is a drive or filesystem root.
///
/// Pure so the refusals can be tested without a filesystem, which is not
/// something to arrange by hand for a delete.
pub fn delete_guard(root: &std::path::Path) -> Option<String> {
    if !root.is_absolute() {
        return Some(format!("{} is not an absolute path", root.display()));
    }
    if root.parent().is_none_or(|p| p.as_os_str().is_empty()) {
        return Some(format!("{} is a drive root", root.display()));
    }
    None
}

/// Rewrite only the `name` field of a `.arcproj` document, leaving every other
/// key -- and their order -- exactly as found.
///
/// Order survives because serde_json is built with `preserve_order`. Keys we do
/// not understand survive because the document is edited as a `Value` rather
/// than parsed into a struct and re-emitted: a project may carry fields a newer
/// engine added, and a rename must not silently delete them.
pub fn rename_in_manifest(text: &str, new_name: &str) -> Result<String, String> {
    let mut doc: serde_json::Value =
        serde_json::from_str(text).map_err(|e| format!("the .arcproj is not valid JSON: {e}"))?;
    let obj = doc
        .as_object_mut()
        .ok_or_else(|| "the .arcproj is not a JSON object".to_string())?;
    obj.insert("name".to_string(), serde_json::Value::String(new_name.to_string()));
    serde_json::to_string_pretty(&doc).map_err(|e| e.to_string())
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
    use std::path::Path;

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
    fn split_args_splits_on_whitespace() {
        assert_eq!(split_args("--backend vulkan"), vec!["--backend", "vulkan"]);
        assert_eq!(split_args("  --a   --b  "), vec!["--a", "--b"]);
        assert!(split_args("").is_empty());
        assert!(split_args("   ").is_empty());
    }

    #[test]
    fn split_args_keeps_a_quoted_token_together() {
        // The case this exists for: a path or a level name with a space in it.
        assert_eq!(
            split_args(r#"--scene "My Level" --frames 5"#),
            vec!["--scene", "My Level", "--frames", "5"],
        );
    }

    #[test]
    fn split_args_strips_quotes_and_joins_adjacent_pieces() {
        // `--path="C:/a b"` is one argv token, exactly as a shell would pass it.
        assert_eq!(split_args(r#"--path="C:/a b""#), vec!["--path=C:/a b"]);
    }

    #[test]
    fn split_args_keeps_an_empty_quoted_token() {
        // "" is a real, meaningful argument; dropping it would silently shift
        // every positional argument after it.
        assert_eq!(split_args(r#"--name "" --x"#), vec!["--name", "", "--x"]);
    }

    #[test]
    fn split_args_tolerates_an_unterminated_quote() {
        // Half-typed input must not lose the text, and must never panic.
        assert_eq!(split_args(r#"--scene "My Lev"#), vec!["--scene", "My Lev"]);
    }

    #[test]
    fn name_error_accepts_ordinary_names() {
        assert_eq!(name_error("MyGame"), None);
        assert_eq!(name_error("My Game"), None);
        assert_eq!(name_error("3d-demo"), None);
        assert_eq!(name_error("My.Game"), None);
    }

    #[test]
    fn name_error_rejects_what_windows_rejects() {
        // This is the GATE, not the hint: rename_project turns the value into a
        // directory name, so every one of these has to be refused here even if
        // the UI never sends it.
        assert!(name_error("").is_some());
        assert!(name_error("   ").is_some());
        assert!(name_error("a/b").is_some());
        assert!(name_error("a\\b").is_some());
        assert!(name_error("a:b").is_some());
        assert!(name_error("a*b").is_some());
        assert!(name_error("a\u{7}b").is_some(), "control characters");
        assert!(name_error("trailing.").is_some());
        assert!(name_error("CON").is_some());
        assert!(name_error("nul.txt").is_some());
        assert!(name_error(&"x".repeat(65)).is_some());
    }

    #[test]
    fn name_error_accepts_a_trailing_space_because_it_is_trimmed_away() {
        // Not an oversight: callers use the TRIMMED name, so "MyGame " creates
        // the folder "MyGame" -- there is nothing for Windows to strip and
        // nothing for the user to be surprised by.
        assert_eq!(name_error("MyGame "), None);
    }

    #[test]
    fn name_error_refuses_dot_dot_traversal() {
        // ".." has no illegal character and is not reserved, but as a folder
        // name it would move the project UP a level. It ends with a dot, which
        // is what catches it -- asserted explicitly so that rule cannot be
        // relaxed without this failing.
        assert!(name_error("..").is_some());
        assert!(name_error(".").is_some());
    }

    #[test]
    fn arcproj_in_args_finds_the_file_wherever_it_sits() {
        assert_eq!(
            arcproj_in_args(["C:\\hub\\arcane_hub.exe", "D:\\G\\My.arcproj"]),
            Some("D:\\G\\My.arcproj"),
            "the exe in argv[0] must never shadow the file"
        );
        assert_eq!(arcproj_in_args(["D:/G/My.ARCPROJ"]), Some("D:/G/My.ARCPROJ"));
        assert_eq!(arcproj_in_args(["C:\\hub\\arcane_hub.exe"]), None);
        assert_eq!(arcproj_in_args([]), None);
    }

    #[test]
    fn copy_name_numbers_from_the_second_copy_onward() {
        assert_eq!(copy_name("MyGame", 1), "MyGame Copy");
        assert_eq!(copy_name("MyGame", 2), "MyGame Copy 2");
        assert_eq!(copy_name("MyGame", 17), "MyGame Copy 17");
    }

    #[test]
    fn skip_in_duplicate_names_build_output_and_the_repo() {
        assert!(skip_in_duplicate("Binaries"));
        assert!(skip_in_duplicate("Intermediate"));
        assert!(skip_in_duplicate(".git"));
        assert!(skip_in_duplicate("binaries"), "Windows paths are case-insensitive");
        assert!(!skip_in_duplicate("Content"), "content is the point of the copy");
        assert!(!skip_in_duplicate("Source"), "source rebuilds the skipped outputs");
        assert!(!skip_in_duplicate("Config"));
    }

    #[test]
    fn delete_guard_passes_an_ordinary_project_folder() {
        assert_eq!(delete_guard(Path::new("D:\\Games\\MyGame")), None);
        assert_eq!(delete_guard(Path::new("C:\\Users\\me\\Projects\\G")), None);
    }

    #[test]
    fn delete_guard_refuses_a_drive_root() {
        assert!(delete_guard(Path::new("C:\\")).is_some());
        assert!(delete_guard(Path::new("D:/")).is_some());
    }

    #[test]
    fn delete_guard_refuses_a_relative_path() {
        // Resolved against the Hub's working directory, which is not anywhere
        // the user chose.
        assert!(delete_guard(Path::new("MyGame")).is_some());
        assert!(delete_guard(Path::new("..\\MyGame")).is_some());
        assert!(delete_guard(Path::new("")).is_some());
    }

    #[test]
    fn delete_guard_allows_a_folder_one_level_under_a_drive() {
        // C:\Projects is a perfectly ordinary place to keep a project; the
        // guard is about roots, not about depth.
        assert_eq!(delete_guard(Path::new("C:\\Projects")), None);
    }

    #[test]
    fn rename_in_manifest_replaces_only_the_name() {
        let before = r#"{"formatVersion":1,"name":"Old","description":"d",
                         "engine":{"abi":7},"gameModule":"Old.dll","plugins":[],
                         "bootScene":"game://s.ascene"}"#;
        let after = rename_in_manifest(before, "New").unwrap();
        let v: serde_json::Value = serde_json::from_str(&after).unwrap();
        assert_eq!(v["name"], "New");
        assert_eq!(v["description"], "d");
        assert_eq!(v["engine"]["abi"], 7);
        assert_eq!(v["bootScene"], "game://s.ascene");
        // NOT rewritten on purpose: the game module is build output named by the
        // project's own build scripts, and the Hub cannot rebuild it.
        assert_eq!(v["gameModule"], "Old.dll");
    }

    #[test]
    fn rename_in_manifest_keeps_unknown_keys_and_their_order() {
        let before = r#"{"formatVersion":1,"name":"Old","zzzFutureField":42,"aaa":1}"#;
        let after = rename_in_manifest(before, "New").unwrap();
        let v: serde_json::Value = serde_json::from_str(&after).unwrap();
        assert_eq!(v["zzzFutureField"], 42, "a field a newer engine added must survive");
        // preserve_order: without it serde_json sorts keys and the whole file
        // comes back reordered for a one-field edit.
        let keys: Vec<&str> = v.as_object().unwrap().keys().map(|s| s.as_str()).collect();
        assert_eq!(keys, ["formatVersion", "name", "zzzFutureField", "aaa"]);
    }

    #[test]
    fn rename_in_manifest_adds_the_name_when_it_is_missing() {
        // FromJson requires `name`, so a manifest without one is already broken;
        // writing it is a repair, not a surprise.
        let after = rename_in_manifest(r#"{"formatVersion":1}"#, "New").unwrap();
        let v: serde_json::Value = serde_json::from_str(&after).unwrap();
        assert_eq!(v["name"], "New");
    }

    #[test]
    fn rename_in_manifest_refuses_a_document_it_cannot_edit() {
        assert!(rename_in_manifest("not json", "New").is_err());
        assert!(rename_in_manifest("[1,2,3]", "New").is_err());
    }

    #[test]
    fn manifest_survives_a_backslash_in_the_name() {
        let text = manifest_json("A\\B", 7).unwrap();
        let v: serde_json::Value = serde_json::from_str(&text).expect("must stay valid JSON");
        assert_eq!(v["name"], "A\\B");
    }
}
