# Jenkins CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the Jenkins CI environment from the 2026-06-11 design spec — multibranch pipeline, Windows + Linux agent lanes, ephemeral CI Postgres — fully operational on the dev box now (Phase 0) and migratable to the X1-255 by checklist when it arrives (Phase 1).

**Architecture:** Jenkins LTS controller (Windows service) + inbound agent `win-gpu` (auto-logon interactive session, label `windows gpu`) + inbound agent `linux-1` (Docker container from an in-repo Dockerfile, label `linux`). One declarative `Jenkinsfile` at the repo root drives every branch: fast tier (builds + fast suites + client gate) everywhere, AccountTests tier on `main`/`milestone/*` against a project-scoped ephemeral Postgres on port 5433. All reusable logic lives in-repo under `ci/`; only credentials and Jenkins home live on the box.

**Tech Stack:** Jenkins LTS (Windows MSI), declarative Pipeline, GitHub Branch Source (polling, auto commit-status), JUnit + Pipeline Utility Steps + Discord Notifier plugins, Docker Desktop (Linux containers/WSL2), Catch2 JUnit reporter, vswhere-resolved MSBuild.

**Spec:** `docs/superpowers/specs/2026-06-11-jenkins-ci-design.md`

---

## Standing constraints (every task)

- **NEVER touch the dev database.** All CI database commands MUST carry `-p aphelyon_ci`. A bare `docker compose down -v` in `Server/` destroys the dev volume — forbidden. Verify the `-p` flag is present before running any `down -v`.
- On the dev box, do NOT run pipeline builds while the game services (Auth/Account/Combat) are up locally, and don't rebuild the Server solution while a local AccountTests.exe is running (file lock). AccountTests takes ~17 min and prints nothing until done — flat CPU for 10+ min is stuck, steady CPU growth is normal.
- New files: UTF-8 without BOM, ASCII-only comments. Use Write/Edit tools, not shell redirection.
- Commit after every task, `type(scope):` convention.
- Secrets (GitHub PAT, Discord webhook, agent secrets) never enter the repo. If a command line would contain a secret, type it interactively or read it from an env var the user sets — never commit it, never echo it into a tracked file.

## File structure

```
Jenkinsfile                          NEW — the multibranch pipeline (repo root)
ci/
├── README.md                        NEW — controller setup + credential checklist + X1 migration
├── provision-agent.ps1              NEW — idempotent Windows agent provisioning
├── linux-agent.Dockerfile           NEW — Linux lane agent image
├── docker-compose.ci.yml            NEW — CI Postgres override (container name)
└── msbuild.cmd                      NEW — vswhere-resolved MSBuild wrapper (host-agnostic)
Server/Account/tests/Helpers/TestDb.hpp     NEW — APHELYON_TEST_DB_URL override helper
Server/Account/tests/Helpers/TestDbTest.cpp NEW — its test
Server/Account/tests/Integration/IntegrationDbFixture.hpp  MODIFIED — use TestDb
Server/Account/tests/Integration/ConnectionPoolTest.cpp    MODIFIED — use TestDb
CLAUDE.md                            MODIFIED — CI section
```

---

### Task 1: `APHELYON_TEST_DB_URL` override for integration tests (TDD)

The integration suites compile in `postgresql://aphelyon:aphelyon@localhost:5432/aphelyon`. CI points them at port 5433 via an env var; dev-box behavior is unchanged when the var is unset.

**Files:**
- Create: `Server/Account/tests/Helpers/TestDb.hpp`
- Create: `Server/Account/tests/Helpers/TestDbTest.cpp`
- Modify: `Server/Account/tests/Integration/IntegrationDbFixture.hpp:63-67`
- Modify: `Server/Account/tests/Integration/ConnectionPoolTest.cpp:17-18`

- [ ] **Step 1: Write the failing test** — `Server/Account/tests/Helpers/TestDbTest.cpp`:

```cpp
// TestDbUrl() resolution order: APHELYON_TEST_DB_URL env var, else the
// documented dev default. Read per-call (not cached) so tests can mutate
// the environment; this is test-only infrastructure, perf is irrelevant.

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include "Helpers/TestDb.hpp"

TEST_CASE("TestDbUrl falls back to the dev default when unset", "[helpers]")
{
    _putenv_s("APHELYON_TEST_DB_URL", "");
    REQUIRE(Aphelyon::test::TestDbUrl() ==
            "postgresql://aphelyon:aphelyon@localhost:5432/aphelyon");
}

TEST_CASE("TestDbUrl honors APHELYON_TEST_DB_URL", "[helpers]")
{
    _putenv_s("APHELYON_TEST_DB_URL",
              "postgresql://aphelyon:aphelyon@localhost:5433/aphelyon");
    REQUIRE(Aphelyon::test::TestDbUrl() ==
            "postgresql://aphelyon:aphelyon@localhost:5433/aphelyon");
    _putenv_s("APHELYON_TEST_DB_URL", ""); // restore for later tests
}
```

- [ ] **Step 2: Build to verify it fails to compile** (header doesn't exist):

```bat
cd D:\dev\starworks\Gacha\Server
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Aphelyon.slnx /p:Configuration=Debug /m /t:AccountTests
```
Expected: FAIL — `Cannot open include file: 'Helpers/TestDb.hpp'`.

- [ ] **Step 3: Write `Server/Account/tests/Helpers/TestDb.hpp`**:

```cpp
#pragma once

// CI seam (2026-06-11 Jenkins CI spec): integration suites connect to
// the dev DB literal by default; CI overrides via APHELYON_TEST_DB_URL
// to reach the ephemeral aphelyon_ci Postgres on port 5433. Distinct
// from APHELYON_DB_CONNECTION (production config, DbConfig.hpp) on
// purpose -- tests must never accidentally honor production config.

#include <cstdlib>
#include <string>

namespace Aphelyon::test {

    inline std::string TestDbUrl()
    {
        const char* env = std::getenv("APHELYON_TEST_DB_URL");
        if (env && env[0] != '\0')
            return std::string(env);
        return "postgresql://aphelyon:aphelyon@localhost:5432/aphelyon";
    }

} // namespace Aphelyon::test
```

- [ ] **Step 4: Switch the two literal sites.**

`IntegrationDbFixture.hpp` — replace:
```cpp
    static constexpr const char* kConn =
        "postgresql://aphelyon:aphelyon@localhost:5432/aphelyon";

    IntegrationDbFixture()
        : pool(kConn)
```
with:
```cpp
    IntegrationDbFixture()
        : pool(Aphelyon::test::TestDbUrl())
```
and add `#include "Helpers/TestDb.hpp"` to its include block. Grep the file for other `kConn` uses first (`grep -n kConn IntegrationDbFixture.hpp`) and route every one through `TestDbUrl()`. Check `Server/Account/tests/Persistence/Jsonb/JsonbTraitRoundTripTest.cpp` too — it references the fixture's connection in a comment; if it uses `kConn` in code, switch it the same way.

`ConnectionPoolTest.cpp` — replace its file-local literal (line ~18) with a call to `Aphelyon::test::TestDbUrl()` (add the same include; if the literal is a `constexpr const char*`, change the declaration to `static const std::string kConn = Aphelyon::test::TestDbUrl();`).

- [ ] **Step 5: Build AccountTests, run the helper tests + the integration suite**

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild.exe" ...as above... /t:AccountTests
Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[helpers]"
```
Expected: 2 test cases pass. Then run the FULL suite (env var unset → dev DB on 5432, container must be up, services down):
```bat
Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
```
Expected: `All tests passed (1286 assertions in 192 test cases)` — baseline 190 + the 2 new ones (assertion count grows by 2 to 1288; record actuals).

- [ ] **Step 6: Commit**

```bash
git add Server/Account/tests/Helpers/TestDb.hpp Server/Account/tests/Helpers/TestDbTest.cpp Server/Account/tests/Integration/IntegrationDbFixture.hpp Server/Account/tests/Integration/ConnectionPoolTest.cpp
git commit -m "test(account): APHELYON_TEST_DB_URL override for integration DB target"
```

---

### Task 2: Ephemeral CI Postgres — override file + full manual drill

Prove the entire CI database story (5433, project-scoped, schema+seed, AccountTests, teardown) by hand before Jenkins automates it.

**Files:**
- Create: `ci/docker-compose.ci.yml`

- [ ] **Step 1: Write `ci/docker-compose.ci.yml`**

```yaml
# CI override for Server/docker-compose.yml. Used ONLY with:
#   docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml ...
# The -p project name isolates the volume (aphelyon_ci_aphelyon_pgdata);
# this file overrides the container name (hardcoded upstream, would clash
# with the dev container on a dev box). Port comes from POSTGRES_PORT=5433.
services:
  postgres:
    container_name: aphelyon_ci_postgres
```

- [ ] **Step 2: Bring up the CI database (dev DB stays up and untouched)**

```powershell
Set-Location D:\dev\starworks\Gacha
$env:POSTGRES_PORT = "5433"
docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml up -d --wait
docker ps --format "{{.Names}} {{.Ports}}" | Select-String postgres
```
Expected: BOTH `aphelyon_postgres ... 5432` and `aphelyon_ci_postgres ... 5433` listed.

- [ ] **Step 3: Apply schema + seed to the CI database**

```powershell
docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/schema.sql
docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/seed.sql
```
Expected: both exit 0. If schema fails with `schema "partman" does not exist`, the image wasn't built from `Dockerfile.postgres` — rebuild with `docker compose -p aphelyon_ci ... build postgres`.

- [ ] **Step 4: Run AccountTests against 5433**

```powershell
$env:APHELYON_TEST_DB_URL = "postgresql://aphelyon:aphelyon@localhost:5433/aphelyon"
Set-Location Server\bin\Debug-windows-x86_64\AccountTests
.\AccountTests.exe
```
Expected: all pass (~17 min; counts match Task 1 Step 5). This proves Task 1's override end-to-end.

- [ ] **Step 5: Teardown — project-scoped, and verify the dev DB survived**

```powershell
Remove-Item Env:\APHELYON_TEST_DB_URL
Set-Location D:\dev\starworks\Gacha
docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml down -v
docker volume ls | Select-String aphelyon
docker compose -f Server/docker-compose.yml exec -T postgres psql -U aphelyon -d aphelyon -c "SELECT count(*) FROM accounts;"
```
Expected: `aphelyon_ci_*` volumes GONE, `aphelyon_pgdata` (dev) PRESENT, and the dev-DB query returns a row count (dev data intact).

- [ ] **Step 6: Commit**

```bash
git add ci/docker-compose.ci.yml
git commit -m "build(ci): ephemeral CI postgres override - project-scoped, port 5433"
```

---

### Task 3: Host-agnostic MSBuild wrapper + Linux agent image

**Files:**
- Create: `ci/msbuild.cmd`
- Create: `ci/linux-agent.Dockerfile`

- [ ] **Step 1: Write `ci/msbuild.cmd`** (the Jenkinsfile must not hardcode a VS edition path — dev box has Community, X1 will likely get Build Tools):

```bat
@echo off
:: Locate MSBuild via vswhere (ships with any VS/Build Tools install) and
:: forward all arguments. Keeps the Jenkinsfile host-agnostic.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere not found at "%VSWHERE%" - is Visual Studio installed?
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo ERROR: MSBuild not found via vswhere.
    exit /b 1
)
"%MSBUILD%" %*
```

- [ ] **Step 2: Smoke it**

```bat
D:\dev\starworks\Gacha\ci\msbuild.cmd -version
```
Expected: prints the MSBuild version banner, exit 0.

- [ ] **Step 3: Write `ci/linux-agent.Dockerfile`**

```dockerfile
# Jenkins Linux lane agent (label: linux). Real Linux userspace under
# Docker Desktop/WSL2 today; the same image runs unchanged on any future
# Linux host (the migration seam from the CI design spec).
# Build:  docker build -t aphelyon/jenkins-linux-agent -f ci/linux-agent.Dockerfile ci
# Run:    see ci/README.md (needs controller URL + agent secret)
FROM jenkins/inbound-agent:latest-jdk21

USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        make \
        libpq-dev \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

# premake5 Linux binary, pinned to the version vendored for Windows
# (ThirdParty/premake5 is 5.0-beta8 per ThirdParty/README.md).
RUN curl -fsSL -o /tmp/premake.tar.gz \
        https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-linux.tar.gz \
    && tar -xzf /tmp/premake.tar.gz -C /usr/local/bin premake5 \
    && rm /tmp/premake.tar.gz \
    && premake5 --version

USER jenkins
```

- [ ] **Step 4: Build the image and smoke the toolchain**

```powershell
Set-Location D:\dev\starworks\Gacha
docker build -t aphelyon/jenkins-linux-agent -f ci/linux-agent.Dockerfile ci
docker run --rm --entrypoint bash aphelyon/jenkins-linux-agent -c "g++ --version | head -1; clang++ --version | head -1; premake5 --version"
```
Expected: gcc, clang, and premake version lines print, exit 0. (If the premake URL 404s, list available assets at https://github.com/premake/premake-core/releases and pin the closest beta; record the change in the Dockerfile comment.)

- [ ] **Step 5: Commit**

```bash
git add ci/msbuild.cmd ci/linux-agent.Dockerfile
git commit -m "build(ci): vswhere msbuild wrapper + linux lane agent image"
```

---

### Task 4: The Jenkinsfile

**Files:**
- Create: `Jenkinsfile` (repo root)

- [ ] **Step 1: Write `Jenkinsfile`**

```groovy
// Aphelyon CI - multibranch pipeline (spec: docs/superpowers/specs/2026-06-11-jenkins-ci-design.md)
// Tiers: every branch -> builds + fast suites + client gate (~15-18 min);
//        main + milestone/* -> also AccountTests vs ephemeral CI postgres (~+17 min).
// Stage order is cheapest-failure-first so compile breaks report in ~3 min.

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
                stage('Generate') {
                    steps {
                        bat 'cd Server && call GenerateProjects.bat'
                        bat 'cd Arcane && call GenerateProjects.bat'
                        bat 'cd Tools  && call GenerateProjects.bat'
                    }
                }
                stage('Build Server') {
                    steps {
                        bat 'ci\\msbuild.cmd Server\\Aphelyon.slnx /p:Configuration=Debug   /m /v:minimal /nologo'
                        bat 'ci\\msbuild.cmd Server\\Aphelyon.slnx /p:Configuration=Release /m /v:minimal /nologo'
                    }
                }
                stage('Build Arcane') {
                    steps {
                        bat 'ci\\msbuild.cmd Arcane\\Arcane.slnx /p:Configuration=Debug   /m /v:minimal /nologo'
                        bat 'ci\\msbuild.cmd Arcane\\Arcane.slnx /p:Configuration=Release /m /v:minimal /nologo'
                        bat 'ci\\msbuild.cmd Arcane\\Arcane.slnx /p:Configuration=Dist    /m /v:minimal /nologo'
                    }
                }
                stage('Build Tools') {
                    steps {
                        bat 'ci\\msbuild.cmd Tools\\AphelyonTools.slnx /p:Configuration=Debug /m /v:minimal /nologo'
                    }
                }
                stage('Fast tests') {
                    steps {
                        bat 'if not exist test-results mkdir test-results'
                        // Each suite runs from its bin dir (fixtures are staged relative to cwd).
                        bat 'cd Server\\bin\\Debug-windows-x86_64\\CommonTests  && CommonTests.exe  -r junit::out=%WORKSPACE%\\test-results\\common-debug.xml'
                        bat 'cd Server\\bin\\Debug-windows-x86_64\\AuthTests    && AuthTests.exe    -r junit::out=%WORKSPACE%\\test-results\\auth-debug.xml'
                        bat 'cd Server\\bin\\Debug-windows-x86_64\\CombatTests  && CombatTests.exe  -r junit::out=%WORKSPACE%\\test-results\\combat-debug.xml'
                        bat 'cd Arcane\\bin\\Debug-windows-x86_64-md\\ArcaneTests   && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-debug.xml'
                        bat 'cd Arcane\\bin\\Release-windows-x86_64-md\\ArcaneTests && ArcaneTests.exe -r junit::out=%WORKSPACE%\\test-results\\arcane-release.xml'
                    }
                }
                stage('Client gate') {
                    steps {
                        bat 'ThirdParty\\love2d\\lovec.exe Client\\src\\tests\\assets_harness'
                        bat 'ThirdParty\\love2d\\lovec.exe Client\\src\\tests\\render_harness'
                        bat 'ThirdParty\\love2d\\lovec.exe Client\\src\\tests\\threading_harness'
                        bat 'ThirdParty\\love2d\\lovec.exe Client\\src\\tests\\world_harness'
                        bat 'ThirdParty\\love2d\\lovec.exe Client\\src\\tests\\physics_harness'
                    }
                }
                stage('AccountTests (ephemeral DB)') {
                    when { anyOf { branch 'main'; branch 'milestone/*' } }
                    environment {
                        POSTGRES_PORT        = '5433'
                        APHELYON_TEST_DB_URL = 'postgresql://aphelyon:aphelyon@localhost:5433/aphelyon'
                        CI_COMPOSE           = 'docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml'
                    }
                    steps {
                        bat '%CI_COMPOSE% up -d --wait --build'
                        bat '%CI_COMPOSE% exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/schema.sql'
                        bat '%CI_COMPOSE% exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/seed.sql'
                        bat 'cd Server\\bin\\Debug-windows-x86_64\\AccountTests && AccountTests.exe -r junit::out=%WORKSPACE%\\test-results\\account-debug.xml'
                    }
                    post {
                        // ALWAYS scoped to -p aphelyon_ci. A bare down -v would
                        // destroy a dev database if one exists on this host.
                        always {
                            bat 'docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml down -v'
                        }
                    }
                }
            }
            post {
                always {
                    junit allowEmptyResults: true, testResults: 'test-results/*.xml'
                }
            }
        }

        stage('Deploy') {
            // Empty named hook per the CI design spec: CD activates with
            // milestone/* branches once a deployable artifact exists.
            when { branch 'milestone/*' }
            agent { label 'windows && gpu' }
            steps {
                echo 'Deploy hook: no deployable artifact yet (CD deferred by design).'
            }
        }

        stage('Linux lane check') {
            // Runs only when a linux agent is online (lane plumbing proof);
            // skipped silently otherwise so an offline container never blocks
            // a build. Real Linux build/test stages activate with the Linux
            // port milestone (out of scope per the CI design spec).
            when { expression { !nodesByLabel(label: 'linux', offline: false).isEmpty() } }
            agent { label 'linux' }
            steps {
                sh 'g++ --version | head -1 && clang++ --version | head -1 && premake5 --version && echo LINUX LANE READY'
            }
        }
    }

    post {
        failure {
            withCredentials([string(credentialsId: 'discord-webhook', variable: 'DISCORD_URL')]) {
                discordSend webhookURL: env.DISCORD_URL,
                            title: "FAILED: ${env.JOB_NAME} #${env.BUILD_NUMBER}",
                            description: "Branch ${env.BRANCH_NAME} is red.",
                            link: env.BUILD_URL, result: 'FAILURE'
            }
        }
        fixed {
            withCredentials([string(credentialsId: 'discord-webhook', variable: 'DISCORD_URL')]) {
                discordSend webhookURL: env.DISCORD_URL,
                            title: "RECOVERED: ${env.JOB_NAME} #${env.BUILD_NUMBER}",
                            description: "Branch ${env.BRANCH_NAME} is green again.",
                            link: env.BUILD_URL, result: 'SUCCESS'
            }
        }
    }
}
```

Notes for the implementer:
- GitHub commit statuses are published automatically by the GitHub Branch Source plugin for multibranch jobs — no pipeline code needed.
- `nodesByLabel` comes from Pipeline Utility Steps (plugin list, Task 5).
- `discordSend` comes from the Discord Notifier plugin. If the credential `discord-webhook` doesn't exist yet, the post blocks fail — create credentials (Task 6) before the first build.
- Catch2 v3 JUnit reporter syntax `-r junit::out=<file>` writes XML and suppresses console output; that's fine, the XML is the record.

- [ ] **Step 2: Syntax-sanity the Jenkinsfile** — no linter is available pre-controller; re-read it checking: every `stage` has exactly one of `steps`/`stages`, every `bat` is single-quoted (no Groovy interpolation of `%VARS%`), the only `${...}` interpolations are in the Discord strings (env-only, no secrets — `DISCORD_URL` is referenced as `env.DISCORD_URL` inside `withCredentials`, acceptable).

- [ ] **Step 3: Commit**

```bash
git add Jenkinsfile
git commit -m "build(ci): multibranch Jenkinsfile - tiered stages, ephemeral DB, linux lane"
```

---

### Task 5: Provisioning script + CI README

**Files:**
- Create: `ci/provision-agent.ps1`
- Create: `ci/README.md`

- [ ] **Step 1: Write `ci/provision-agent.ps1`**

```powershell
<#
Idempotent Windows build-agent provisioning (CI design spec 2026-06-11).
Run as Administrator. Detection-based: skips anything already installed,
so it is safe on the dev box (everything present) and does real work on
a fresh X1-255. Installs/checks: Git, Temurin JRE 21 (agent runtime),
vcpkg (+VCPKG_ROOT), and verifies MSBuild + Docker, which require manual
installs (VS installer / Docker Desktop) when missing.

Usage:
  .\provision-agent.ps1                      # provision toolchain only
  .\provision-agent.ps1 -ControllerUrl http://localhost:8080 -NodeName win-gpu -Secret <secret>
                                             # ...also installs the agent
                                             # startup command (auto-logon
                                             # session launch)
#>
param(
    [string]$ControllerUrl,
    [string]$NodeName,
    [string]$Secret
)

$ErrorActionPreference = "Stop"
function Step($msg) { Write-Host "== $msg" -ForegroundColor Cyan }

Step "Git"
if (Get-Command git -ErrorAction SilentlyContinue) {
    git --version
} else {
    winget install --id Git.Git -e --accept-source-agreements --accept-package-agreements
}

Step "Java runtime (agent)"
if (Get-Command java -ErrorAction SilentlyContinue) {
    java -version
} else {
    winget install --id EclipseAdoptium.Temurin.21.JRE -e --accept-source-agreements --accept-package-agreements
}

Step "MSBuild (via vswhere)"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if ((Test-Path $vswhere) -and (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe")) {
    Write-Host "MSBuild present."
} else {
    Write-Warning "MSBuild NOT found. Install Visual Studio 2026 Build Tools (or Community) with the 'Desktop development with C++' workload, then re-run."
}

Step "Docker"
if (Get-Command docker -ErrorAction SilentlyContinue) {
    docker --version
} else {
    Write-Warning "Docker NOT found. Install Docker Desktop (WSL2 backend, Linux containers), then re-run."
}

Step "vcpkg"
if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) {
    Write-Host "vcpkg at $env:VCPKG_ROOT"
} else {
    git clone https://github.com/microsoft/vcpkg C:\vcpkg
    & C:\vcpkg\bootstrap-vcpkg.bat
    setx VCPKG_ROOT C:\vcpkg /M
    Write-Warning "VCPKG_ROOT set machine-wide; restart the shell before building."
}

if ($ControllerUrl -and $NodeName -and $Secret) {
    Step "Jenkins inbound agent (interactive-session launch)"
    $agentDir = "C:\jenkins-agent"
    New-Item -ItemType Directory -Force $agentDir | Out-Null
    Invoke-WebRequest "$ControllerUrl/jnlpJars/agent.jar" -OutFile "$agentDir\agent.jar"
    $cmd = "@echo off`r`ncd /d $agentDir`r`njava -jar agent.jar -url $ControllerUrl -name $NodeName -secret $Secret -workDir $agentDir`r`n"
    $startup = [Environment]::GetFolderPath("Startup")
    Set-Content -Path "$startup\jenkins-agent.cmd" -Value $cmd -Encoding ascii
    Write-Host "Agent launch command installed to the Startup folder."
    Write-Warning "Configure auto-logon manually (netplwiz or Sysinternals Autologon) so the agent session - and GPU access - survives reboots. The agent secret is in the startup script; the account should be a dedicated low-privilege user on a CI-only box."
} else {
    Write-Host "(Agent launch not configured - pass -ControllerUrl/-NodeName/-Secret to set it up.)"
}

Step "Done"
```

- [ ] **Step 2: Run it on the dev box (toolchain mode, no agent params)**

```powershell
powershell -ExecutionPolicy Bypass -File D:\dev\starworks\Gacha\ci\provision-agent.ps1
```
Expected: every section reports "present" (dev box has it all); no installs occur. This validates the detection paths.

- [ ] **Step 3: Write `ci/README.md`** — the controller checklist (this IS the X1 migration document). Content:

```markdown
# CI operations

Design: `docs/superpowers/specs/2026-06-11-jenkins-ci-design.md`.
Everything reusable is in this directory; this README is the controller
checklist. Setting up a new box = provision script + this checklist.

## Controller install (Windows)

1. Install Jenkins LTS via the Windows installer (https://www.jenkins.io/download/) —
   runs as a service on port 8080, bundles its JRE. Local admin account on first run.
2. Plugins (Manage Jenkins -> Plugins): **Git**, **GitHub Branch Source**,
   **Pipeline** (workflow-aggregator), **JUnit**, **Timestamper**,
   **Pipeline Utility Steps**, **Discord Notifier**.
3. Manage Jenkins -> Nodes -> Built-In Node -> set **0 executors**.

## Credentials (Manage Jenkins -> Credentials -> Global)

| ID | Kind | Content |
|---|---|---|
| `github-pat` | Username with password | GitHub username + a fine-grained PAT for `T3mps/Aphelyon` with **Contents: read** and **Commit statuses: read/write** |
| `discord-webhook` | Secret text | Discord channel webhook URL (Server Settings -> Integrations -> Webhooks) |

## Agents

### win-gpu (Windows, interactive session)
1. Manage Jenkins -> Nodes -> New Node: name `win-gpu`, permanent agent,
   remote root `C:\jenkins-agent`, **labels: `windows gpu`**, 1 executor,
   launch: "Launch agent by connecting it to the controller". Copy the secret.
2. On the agent machine, as admin:
   `ci\provision-agent.ps1 -ControllerUrl http://<controller>:8080 -NodeName win-gpu -Secret <secret>`
3. Configure auto-logon (netplwiz / Sysinternals Autologon) and sign in once.
   The agent must run in the interactive session for GPU visibility (M1+ device tests).

### linux-1 (container)
1. New Node: name `linux-1`, remote root `/home/jenkins/agent`, **label: `linux`**,
   1 executor, same inbound launch method. Copy the secret.
2. Build + run on any Docker host (the CI box itself today):
   ```
   docker build -t aphelyon/jenkins-linux-agent -f ci/linux-agent.Dockerfile ci
   docker run -d --restart unless-stopped --name jenkins-linux-agent ^
     -e JENKINS_URL=http://host.docker.internal:8080 ^
     -e JENKINS_AGENT_NAME=linux-1 -e JENKINS_SECRET=<secret> ^
     -e JENKINS_AGENT_WORKDIR=/home/jenkins/agent ^
     aphelyon/jenkins-linux-agent
   ```
   (On a real Linux host, replace host.docker.internal with the controller address.)

## The job

New Item -> **Multibranch Pipeline** -> name `Aphelyon`.
- Branch Sources -> GitHub: credentials `github-pat`, repo URL
  `https://github.com/T3mps/Aphelyon.git`; behaviors: "Discover branches".
- Build Configuration: by Jenkinsfile, path `Jenkinsfile`.
- Scan Repository Triggers: periodically, **2 minutes**.
- Orphaned Item Strategy: discard old items, max 30 days / 30 items.
- (Per-branch workspaces are multi-GB; orphan cleanup is what keeps the disk sane.)

Commit statuses on GitHub are published automatically by GitHub Branch Source.

## Migrating the controller to a new box (e.g. the X1-255)

1. Run `ci\provision-agent.ps1` on the new box; install VS Build Tools +
   Docker Desktop when the script flags them.
2. Follow this README top to bottom on the new box (controller, plugins,
   credentials, both agents, job). ~30 minutes.
3. Point of proof: first `main` build goes green on the new box.
4. Delete the old controller (uninstall service); optionally keep the old
   box's agent registered, offline by default, for ad-hoc builds.

## Operational notes

- NEVER run `docker compose down -v` against the CI checkout without
  `-p aphelyon_ci`. On a box that also hosts a dev database this is the
  difference between cleaning CI state and destroying dev state.
- On a shared dev box: don't run builds while the game services are up
  (AccountTests contends on the internal RPC ports and wedges).
- AccountTests stage takes ~17 min with no output until the end. Normal.
```

- [ ] **Step 4: Commit**

```bash
git add ci/provision-agent.ps1 ci/README.md
git commit -m "build(ci): agent provisioning script + controller/ops README"
```

---

### Task 6: Controller install on the dev box (Phase 0) — interactive

This task needs the user for two inputs (PAT, Discord webhook). Everything else follows `ci/README.md` exactly — this is also the first validation that the README is sufficient.

- [ ] **Step 1:** Download + install Jenkins LTS (Windows MSI) on the dev box; complete first-run (install suggested plugins is fine, the explicit list comes next), create the admin user.
- [ ] **Step 2:** Install the plugin list from `ci/README.md` (Git, GitHub Branch Source, Pipeline, JUnit, Timestamper, Pipeline Utility Steps, Discord Notifier). Restart Jenkins.
- [ ] **Step 3:** Set built-in node to 0 executors.
- [ ] **Step 4 (USER):** Create the fine-grained GitHub PAT (repo `T3mps/Aphelyon`, Contents: read, Commit statuses: read/write) and a Discord webhook URL; enter both into Jenkins credentials as `github-pat` and `discord-webhook` per the README. **Neither value is pasted into the chat, the repo, or any file outside Jenkins.**
- [ ] **Step 5:** Verify: Manage Jenkins shows no plugin errors; credentials list shows both IDs.

(No commit — nothing in-repo changes.)

---

### Task 7: Both agents online (Phase 0)

- [ ] **Step 1:** Create node `win-gpu` per README (labels `windows gpu`, 1 executor, inbound). Run the provisioning script in agent mode with the node's secret:
```powershell
powershell -ExecutionPolicy Bypass -File ci\provision-agent.ps1 -ControllerUrl http://localhost:8080 -NodeName win-gpu -Secret <secret-from-node-page>
```
Then start it for this session without waiting for a reboot: run the generated `jenkins-agent.cmd` from the Startup folder manually.
Expected: node `win-gpu` shows **online** in Manage Jenkins -> Nodes. (Auto-logon configuration is deferred on the dev box — you log in anyway; it becomes mandatory on the X1.)

- [ ] **Step 2:** Create node `linux-1` per README; build and run the container with its secret (command in README).
Expected: node `linux-1` shows **online**.

- [ ] **Step 3:** Sanity: from Manage Jenkins -> Script Console is NOT needed — instead create no job yet; just confirm both nodes idle, 1 executor each.

(No commit.)

---

### Task 8: Multibranch job + first green build on `main`

- [ ] **Step 1:** Create the `Aphelyon` multibranch job per README (GitHub source, `github-pat`, 2-min scan, orphan strategy). First branch indexing should discover `main` and start a build automatically.
- [ ] **Step 2:** Babysit the first build. Likely first-run issues and their fixes (fix in repo, push, let it rebuild):
  - `GenerateProjects.bat` pauses → it must see `_APH_NOPAUSE` (set at the Windows stage level; verify it propagated — if not, change the bat steps to `set _APH_NOPAUSE=1&& call GenerateProjects.bat`).
  - `VCPKG_ROOT not set` from Arcane generate → the agent session inherits user env vars only in an interactive session; if launched from a fresh shell it may lack them. Fix: set VCPKG_ROOT machine-wide (`setx /M`, provisioning already does) and restart the agent.
  - Catch2 JUnit flag rejected → check exact syntax with `CommonTests.exe --help`; v3 accepts `-r junit::out=file` (and `--reporter`). Adjust Jenkinsfile if the vendored build predates multi-arg reporters.
  - `discordSend`/credential failures in post → credential ID mismatch; align with Task 6.
- [ ] **Step 3:** Build goes green end-to-end including AccountTests (~35 min total). Verify on GitHub: the `main` HEAD commit shows a green status from Jenkins. Verify the JUnit trend page shows all suites.
- [ ] **Step 4:** Run a second `main` build immediately (Replay/Build Now) and confirm idempotency: no port-5433 collision, no stray `aphelyon_ci` containers/volumes after (`docker ps -a`, `docker volume ls`), green again.
- [ ] **Step 5:** Commit any Jenkinsfile fixes accumulated during babysitting:
```bash
git add Jenkinsfile ci/
git commit -m "build(ci): first-run fixes from live pipeline bring-up"
```

---

### Task 9: Acceptance battery (spec criteria 2, 3, 4, 5)

- [ ] **Step 1 (fast tier):** `git checkout -b ci-accept-fast && git push -u origin ci-accept-fast`. Expected: indexing discovers it within ~2 min; build runs WITHOUT the AccountTests stage (~15-18 min); branch gets its own GitHub status.
- [ ] **Step 2 (red + Discord):** On the branch, commit a deliberate compile error (e.g. add `static_assert(false, "ci acceptance");` to `Arcane/Core/src/ArcaneCore.cpp`), push. Expected: build fails in ~3-5 min at Build Arcane; red GitHub status; Discord failure message arrives.
- [ ] **Step 3 (recovery):** Revert the breaking commit, push. Expected: green build; Discord "RECOVERED" message.
- [ ] **Step 4 (linux lane):** Confirm the `Linux lane check` stage ran on these builds and logged `LINUX LANE READY`. Then `docker stop jenkins-linux-agent`, trigger a build, confirm the stage is SKIPPED (not hung, not failed); `docker start jenkins-linux-agent` after.
- [ ] **Step 5 (branch cleanup):** Delete the branch (`git push origin --delete ci-accept-fast`, `git branch -D ci-accept-fast`). After the next indexing scan, confirm the job's branch entry is gone/orphan-marked and its workspace is removed from `C:\jenkins-agent\workspace`.
- [ ] **Step 6:** Spec acceptance criteria 1-6 all check out; note results.

---

### Task 10: Docs + memory

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1:** Add a `## CI (Jenkins)` section to CLAUDE.md after the Arcane build section:

```markdown
## CI (Jenkins)

Self-hosted Jenkins (design: `docs/superpowers/specs/2026-06-11-jenkins-ci-design.md`;
ops: `ci/README.md`). Multibranch pipeline over GitHub `main` + feature branches:
every branch builds both workspaces (6 configs) + fast suites + the client harness
gate; `main`/`milestone/*` additionally run AccountTests against an **ephemeral**
CI Postgres (`-p aphelyon_ci`, port 5433, destroyed every run — never the dev DB).
The `Jenkinsfile` is at the repo root; `ci/` holds the agent provisioning script,
Linux agent image, and compose override. Workflow: push the branch, let CI go
green, then merge. CI DB commands MUST carry `-p aphelyon_ci`.
```

- [ ] **Step 2:** Commit:
```bash
git add CLAUDE.md
git commit -m "docs: CI section - pipeline tiers, ephemeral DB rules"
```

---

### Task 11: Phase 1 — X1-255 migration (DEFERRED until hardware arrives)

No new artifacts — this is the README executed on the new box, in order:

- [ ] Windows 11 Pro first-boot, updates, rename machine (e.g. `aphelyon-ci`).
- [ ] Install VS 2026 Build Tools ("Desktop development with C++") + Docker Desktop (WSL2/Linux containers).
- [ ] Run `ci\provision-agent.ps1` (toolchain mode) — everything else installs/verifies.
- [ ] Follow `ci/README.md` top-to-bottom: controller, plugins, credentials (re-enter PAT + webhook), nodes `win-gpu` (with auto-logon via netplwiz — mandatory here) and `linux-1`, multibranch job.
- [ ] First `main` build green on the X1 = migration proven.
- [ ] Decommission the dev-box controller (uninstall service); optionally re-register the dev box as an offline-by-default second agent.
- [ ] Update `ci/README.md` with any step that proved wrong or missing, commit as `docs(ci): X1 migration corrections`.

---

## Acceptance (mirrors the spec)

1. `main` push → pending → green ~35 min, all stages, dev DB untouched.
2. Feature branch → fast tier only, own status.
3. Deliberate red → Discord failure; revert → green + recovery message.
4. `linux-1` online → lane check passes; offline → stage skips without blocking.
5. JUnit trends per branch; deleted branch's job entry + workspace cleaned.
6. Back-to-back `main` builds leave no stray containers/volumes/ports.
