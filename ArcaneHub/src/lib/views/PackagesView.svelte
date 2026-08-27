<script lang="ts">
  import EmptyState from "$lib/components/EmptyState.svelte";

  // PLACEHOLDER, deliberately marked as one -- and NOT replaced by a registry,
  // deliberately, which is the substantive change here.
  //
  // The design rule for this sidebar was "every entry points at a surface that
  // exists" -- nav aimed at nothing makes a launcher feel like a mockup of
  // itself. This entry was added ahead of its feature by explicit request, so
  // the mitigation is that the surface says plainly that it is not built yet
  // rather than presenting dead controls. There are no fake install buttons
  // here on purpose: a disabled "Install" is a worse lie than a sentence. That
  // reasoning has NOT expired -- see below.
  //
  // WHY THIS IS STILL A PLACEHOLDER (Task 13, plan-b comparator). This view was
  // slated to become registry-driven, reading discovered *.arcpkg manifests in
  // place of the hardcoded array below. It is not, because the inventory that
  // task ran found there is nothing to discover: Servitor -- the one package
  // this list existed to anticipate -- turned out NOT to be a package at all.
  // It partitions into THREE buckets, not two: the engine mode (ships in every
  // build), the consuming project's own authored reference corpus, and per-repo
  // CI glue -- scripts/golden-gate.ps1 plus the Jenkins stage. A doctor can
  // install neither of the first two; the third is excluded by an ARGUED
  // partition, not by that clause -- CI glue is something a repo OPERATES, not
  // something a project ACQUIRES: it arrives by git clone with no version of
  // its own, no project could name it in packages: [], and its prerequisites
  // describe a CI agent's role rather than a project's dependencies. So the
  // honest outcome was to state the rule rather than build a registry over zero
  // entries. Multiplayer remains a real package, and remains unbuilt.
  // See docs/specs/2026-08-25-package-tiering-design.md (the rule, the *.arcpkg
  // format, the doctor contract, and the evidence for the Servitor verdict).
  //
  // CORRECTION, same task. This comment used to say the work to borrow from was
  // "scripts/setup.ps1", as though it were in this repo. It is not: Arcane's
  // scripts/ has no setup.ps1. That orchestrator lives in the APHELYON/GACHA
  // REPO (scripts/setup.ps1 + scripts/doctor.bat, with Setup.exe as a Tauri GUI
  // over it). It is a PRECEDENT and a reference implementation, in another
  // repository -- not a dependency of Arcane and not something to take one on.
  // The discipline it was cited for still binds: when Arcane grows a doctor,
  // the Hub drives that one rather than growing a second one beside it.

  type Requirement = { kind: string; what: string };
  type Package = { name: string; why: string; requires: Requirement[] };

  // The one real package. Not built. Its requirements are the paper-validated
  // manifest from the tiering spec, section 6 -- derived from the Aphelyon
  // Server as it actually exists, not sketched.
  const packages: Package[] = [
    {
      name: "Multiplayer",
      why: "Persistent player state and matchmaking for a project that wants them.",
      requires: [
        { kind: "tool", what: "Docker Desktop 4.x or newer, running" },
        { kind: "tool", what: "MSBuild (Visual Studio 2026, Desktop development with C++)" },
        { kind: "tool", what: "vcpkg, with VCPKG_ROOT set" },
        { kind: "service", what: "PostgreSQL 16 reachable on 127.0.0.1:5432" },
        { kind: "tree", what: "Server/Account/schema.sql" },
        { kind: "tree", what: "vcpkg-triplets/x64-windows-static.cmake" },
        { kind: "env", what: "APHELYON_INTERNAL_SECRET (Release refuses to start without it)" },
      ],
    },
  ];
</script>

<header>
  <h2 class="display view-title">Packages</h2>
  <p class="view-sub">Optional capability, and the dependencies each one needs</p>
</header>

<div class="wrap">
  <EmptyState
    title="Not built yet"
    body="Packages will add optional capability to a project and check that this
          machine has what it needs — a doctor that reports what is missing and
          installs it, rather than leaving you to follow a setup document. The
          format and that doctor's contract are specified; neither is
          implemented, so nothing below has been checked against this machine." />

  <section>
    <h3 class="label">The rule</h3>
    <p class="rule">
      A <strong>mode</strong> ships in every build and has nothing for a doctor
      to check. A <strong>package</strong> adds optional capability
      <em>and</em> carries dependencies a doctor can report on and install.
      Both halves bind: optional capability with no dependencies is a mode.
    </p>
    <p class="hint">
      Playwright is a package; headless Chrome is a mode of Chrome. See
      <span class="mono">docs/specs/2026-08-25-package-tiering-design.md</span>.
    </p>
  </section>

  <section>
    <h3 class="label">Planned</h3>
    {#each packages as p (p.name)}
      <div class="row">
        <div class="txt">
          <p class="t">{p.name}</p>
          <p class="d">{p.why}</p>
          <ul class="reqs">
            {#each p.requires as r (r.what)}
              <li><span class="kind">{r.kind}</span>{r.what}</li>
            {/each}
          </ul>
        </div>
        <span class="tag">Planned</span>
      </div>
    {/each}
    <p class="hint">
      Requirements are listed, not checked — there is no doctor yet. When there
      is, the Hub will drive the same orchestrator the Aphelyon setup wizard
      already uses (in that repository), rather than growing a second one.
    </p>
  </section>

  <section>
    <h3 class="label">Not a package</h3>
    <div class="row">
      <div class="txt">
        <p class="t">Servitor</p>
        <p class="d">
          Drive a project's editor or runtime with no display, and adjudicate
          what it rendered.
        </p>
        <p class="needs">Ships in every build. Needs nothing installed.</p>
      </div>
      <span class="tag">Mode</span>
    </div>
    <p class="hint">
      Listed here because this surface used to claim otherwise. The
      <span class="mono">--headless</span> mode, its capture, its comparator
      and its report JSON all ship unconditionally; what a project adds is a
      blessed reference-image corpus under
      <span class="mono">Verify/References/</span>, which is that project's own
      authored content — a doctor has nothing to report missing and nothing to
      install. The corpus is even produced by the mode itself, via
      <span class="mono">--bless</span>. That makes it state, not a dependency.
    </p>
  </section>
</div>

<style>
  .wrap { display: flex; flex-direction: column; gap: 22px; }
  section { border-top: 1px solid var(--border-soft); padding-top: 14px; }
  h3 { margin: 0 0 10px; }

  .row { display: flex; align-items: flex-start; justify-content: space-between;
         gap: 16px; padding: 13px 14px; background: var(--surface);
         border: 1px solid var(--border-soft); border-radius: var(--r-panel); }
  .txt { min-width: 0; }
  .t { font-size: 13.5px; font-weight: 600; color: var(--text); margin: 0; }
  .d { font-size: 12.5px; color: var(--text-dim); margin: 4px 0 0; line-height: 1.55; }
  .needs { font-size: 11.5px; color: var(--text-dim); margin: 6px 0 0;
           font-family: var(--font-mono); }

  .rule { font-size: 12.5px; color: var(--text); margin: 0; line-height: 1.6; }
  .rule strong { font-weight: 600; }

  .reqs { list-style: none; margin: 9px 0 0; padding: 0;
          display: flex; flex-direction: column; gap: 3px; }
  .reqs li { font-size: 11.5px; color: var(--text-dim);
             font-family: var(--font-mono); line-height: 1.5; }
  /* The kind is the checkable part -- what a doctor would dispatch on -- so it
     reads as a label rather than as prose. Still neutral: nothing here runs. */
  .kind { display: inline-block; min-width: 58px; margin-right: 8px;
          font-size: 10px; letter-spacing: .08em; text-transform: uppercase;
          color: var(--text-dim); opacity: .7; }

  .mono { font-family: var(--font-mono); font-size: .95em; }

  /* Neutral, not the accent: nothing here can be acted on, and the accent is
     reserved for things that can. */
  .tag { flex: none; font-size: 10.5px; letter-spacing: .12em;
         text-transform: uppercase; color: var(--text-dim);
         border: 1px solid var(--border); border-radius: 999px; padding: 3px 9px; }

  .hint { font-size: 12.5px; color: var(--text-dim); margin: 14px 0 0; line-height: 1.6; }
</style>
