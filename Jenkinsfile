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
