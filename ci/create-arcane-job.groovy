// Create the Arcane multibranch job on the same controller as Aphelyon.
// Paste into Manage Jenkins -> Script Console and Run.
//
// Git SCM over the PUBLIC https://github.com/T3mps/Arcane.git -- no PAT.
// The live `github-pat` credential is org-scoped (StarworksDev) and cannot
// see this personal repo; GitHub Branch Source + statuses wait on a
// T3mps-scoped PAT (ci/README.md). Idempotent: existing `Arcane` is left
// alone.

import jenkins.model.Jenkins
import jenkins.branch.BranchSource
import jenkins.plugins.git.GitSCMSource
import jenkins.plugins.git.traits.BranchDiscoveryTrait
import org.jenkinsci.plugins.workflow.multibranch.WorkflowMultiBranchProject
import org.jenkinsci.plugins.workflow.multibranch.WorkflowBranchProjectFactory
import com.cloudbees.hudson.plugins.folder.computed.PeriodicFolderTrigger
import com.cloudbees.hudson.plugins.folder.computed.DefaultOrphanedItemStrategy

def name = 'Arcane'
def jenkins = Jenkins.instance
if (jenkins.getItem(name) != null) {
    println "job '${name}' already exists -- not touching it"
    return
}

def mp = jenkins.createProject(WorkflowMultiBranchProject, name)
mp.setDescription('Arcane engine CI — github.com/T3mps/Arcane (extraction Phase 3). Public git clone; no org PAT.')

def source = new GitSCMSource('https://github.com/T3mps/Arcane.git')
source.setId('arcane-git')
source.setCredentialsId('')
source.setTraits([new BranchDiscoveryTrait()])

mp.getSourcesList().add(new BranchSource(source))

def factory = new WorkflowBranchProjectFactory()
factory.setScriptPath('Jenkinsfile')
mp.setProjectFactory(factory)

mp.addTrigger(new PeriodicFolderTrigger('2m'))
mp.setOrphanedItemStrategy(new DefaultOrphanedItemStrategy(true, '30', '30'))
mp.save()
mp.scheduleBuild2(0)
println "created '${name}' and scheduled a branch index"
