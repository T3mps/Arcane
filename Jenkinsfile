pipeline {
    agent any

    stages {
        stage('Checkout') {
            steps {
                git url: 'https://github.com/T3mps/Arcane', branch: 'main'
            }
        }

        stage('Build') {
            steps {
                sh 'premake5 gmake2'
                sh 'make config=release_x64'
            }
        }

        stage('Archive Artifacts') {
            steps {
                archiveArtifacts artifacts: 'bin/**', fingerprint: true
            }
        }
    }

    post {
        always {
            junit 'test-results/**/*.xml'
        }
    }
}
