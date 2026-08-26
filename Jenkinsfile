// Arcane engine CI - multibranch pipeline over github.com/T3mps/Arcane
// (extraction spec: docs/specs/2026-08-11-arcane-repo-extraction-design.md).
// windows-1 carries a GPU, so ArcaneTests runs INCLUDING [gpu] tags and the
// scripted --frames GPU-verify runs against ReferenceProject.
// Stage order is cheapest-failure-first.

pipeline {
    agent none

    options {
        buildDiscarder(logRotator(numToKeepStr: '30'))
        disableConcurrentBuilds()
        timestamps()
        skipDefaultCheckout(true)
    }

    stages {
        stage('Windows') {
            agent { label 'windows && gpu' }
            environment {
                _APH_NOPAUSE = '1'
            }
            stages {
                stage('Checkout') {
                    steps { checkout scm }
                }
                stage('Provision (vcpkg SDL3)') {
                    // Self-heal: build SDL3 with the repo's static-md overlay
                    // triplet if it is absent from the agent's vcpkg. Guarded on
                    // presence -- the from-source build is expensive.
                    steps {
                        bat 'if not exist "%VCPKG_ROOT%\\installed\\x64-windows-static-md\\lib\\SDL3-static.lib" ( call scripts\\setup-vcpkg-deps.bat ) else ( echo [provision] SDL3 present -- skipping vcpkg build )'
                    }
                }
                stage('Generate') {
                    steps {
                        bat 'call GenerateProjects.bat'
                    }
                }
                stage('Build') {
                    steps {
                        bat 'ci\\msbuild.cmd Arcane.slnx /p:Configuration=Debug   /m /v:minimal /nologo'
                        bat 'ci\\msbuild.cmd Arcane.slnx /p:Configuration=Release /m /v:minimal /nologo'
                        bat 'ci\\msbuild.cmd Arcane.slnx /p:Configuration=Dist    /m /v:minimal /nologo'
                    }
                }
                stage('Tests (incl [gpu])') {
                    steps {
                        bat 'if not exist test-results mkdir test-results'
                        // Suites run FROM the exe dir (fixtures + shaders are
                        // staged relative to it). GPU tests assert
                        // Arcane::RenderErrorCount() == 0 -- validation noise
                        // is a test failure by design.
                        bat 'cd bin\\Debug-windows-x86_64-md\\ArcaneTests   && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-debug.xml'
                        bat 'cd bin\\Release-windows-x86_64-md\\ArcaneTests && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-release.xml'
                    }
                }
                stage('Golden gate') {
                    // Task 12 (plan-b comparator): the HOST-LEVEL half of the
                    // golden-image gate. The [gpu][golden] Catch2 case in the
                    // stage above proves the render path compiles and matches
                    // its own committed reference, but ArcaneTests links
                    // neither RuntimeApp nor EditorApp -- it is silent about
                    // boot, settle, --report/--compare and the CLI. THIS
                    // stage is what actually launches both real hosts, on
                    // both backends, the way an agent would, and fails the
                    // build on any HARD-GATING comparison regressing.
                    //
                    // -AdvisoryLanes ArcaneEditor -- READ scripts/golden-gate.ps1's
                    // "THE ADVISORY LANES" header before touching this. The two
                    // editor lanes are red on two OWED ENGINE DEFECTS this gate
                    // itself found (--settle counts attempts against a
                    // millisecond-denominated condition; the editor's verify
                    // capture enumerates Saved/Diagnostics/, which the editor
                    // writes crash captures into). They still RUN, still print
                    // their full verdicts, and their diffs are still archived
                    // below -- they just do not decide the exit code.
                    //
                    // Shipping them HARD-RED instead would cost more than a red
                    // box. These are SEQUENTIAL nested stages: a failing `bat`
                    // fails this stage and aborts the enclosing stage('Windows'),
                    // so 'ReferenceProject (SDK build)' and 'Scripted GPU-verify'
                    // below -- both pre-existing, both currently working --
                    // would never run again until the owed defects land. It
                    // would also defeat the Release-then-Debug ordering this
                    // very stage calls load-bearing (see below), because Release
                    // is the failing invocation, so the Debug one that restores
                    // the single-slot Binaries/ would never execute.
                    //
                    // THE SWITCH IS THE TRACKING ARTIFACT: when both owed
                    // defects land, delete the argument here and the parameter
                    // in the script. The editor lanes must NEVER be dropped from
                    // the script's $combos -- a lane that is not run cannot tell
                    // anyone when the fix works.
                    //
                    // scripts/golden-gate.ps1 enforces its own precondition
                    // (a fresh ReferenceProject/Binaries/ rebuild for the
                    // configuration it is about to run, since that folder is
                    // a single slot shared across configs) before launching
                    // anything, so no separate step is needed here for that.
                    //
                    // ORDER IS LOAD-BEARING: RELEASE THEN DEBUG, never the
                    // reverse. Whichever config runs LAST is the one that
                    // wins the single Binaries/ slot, and the very next
                    // stage -- "ReferenceProject (SDK build)", pre-existing
                    // and unmodified -- does a PLAIN (non-Rebuild) `msbuild
                    // /p:Configuration=Debug` on it, then "Scripted
                    // GPU-verify" launches the Debug ArcaneRuntime.exe
                    // against whatever that produced. A plain build on an
                    // already-current Debug tree can silently no-op ("all
                    // outputs up-to-date") rather than actually rebuild, so
                    // if this stage left the slot in RELEASE state, that
                    // next stage would hand the Debug host a stale Release
                    // ReferenceGame.dll -- the exact single-slot staleness
                    // class this script's own /t:Rebuild exists to prevent,
                    // reintroduced one stage downstream. Running Release
                    // first and Debug last leaves the slot in the state the
                    // rest of the pipeline already assumes.
                    steps {
                        bat 'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\golden-gate.ps1 -Configuration Release -AdvisoryLanes ArcaneEditor'
                        bat 'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\golden-gate.ps1 -Configuration Debug   -AdvisoryLanes ArcaneEditor'
                    }
                    post {
                        // Diff artifacts land under EACH exe's own directory
                        // (bin/<Config>-windows-x86_64-md/<ArcaneRuntime|
                        // ArcaneEditor>/ReferenceProject/Saved/Verify/*.png --
                        // Saved/ is project-gitignored, so this is the only
                        // way a failing lane's diff image survives the
                        // workspace being wiped for the next build).
                        //
                        // `always`, NOT `failure` -- and that change is a
                        // direct consequence of -AdvisoryLanes above. An
                        // advisory lane fails WITHOUT failing the stage, so a
                        // failure-only archive would silently drop exactly the
                        // artifacts the advisory marker promises to keep: the
                        // editor diffs, on the only builds where they exist.
                        // The cost is a few PNGs archived on a green build;
                        // allowEmptyArchive keeps a genuinely clean run quiet.
                        always {
                            archiveArtifacts artifacts: 'bin/**/ReferenceProject/Saved/Verify/*.png',
                                              allowEmptyArchive: true
                        }
                    }
                }
                stage('ReferenceProject (SDK build)') {
                    // The in-repo external-project consumer: its own premake
                    // workspace over build/arcane.lua -> Binaries/ReferenceGame.dll.
                    steps {
                        bat 'cd ReferenceProject && ..\\ThirdParty\\premake5\\premake5.exe vs2026'
                        bat 'ci\\msbuild.cmd ReferenceProject\\ReferenceProject.slnx /p:Configuration=Debug /m /v:minimal /nologo'
                    }
                }
                stage('Scripted GPU-verify (--frames)') {
                    steps {
                        bat 'bin\\Debug-windows-x86_64-md\\ArcaneRuntime\\ArcaneRuntime.exe --project ReferenceProject --frames 180'
                    }
                }
            }
            post {
                always {
                    junit allowEmptyResults: true, testResults: 'test-results/*.xml'
                }
            }
        }
    }
}
