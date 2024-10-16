pipeline {
    agent any 

    options {
        timestamps()
        timeout(time: 90, unit: 'MINUTES')
    }

    environment {
        CCACHE_DIR = "${env.WORKSPACE}/.ccache"
        BUILD_TYPE = 'Release'
        MINGW_PREFIX = 'x86_64-w64-mingw32'
    }

    stages {
        stage('Prepare Environment') {
            steps {
                sh '''
                sudo apt-get update
                sudo apt-get install -y build-essential ccache cppcheck clang-tidy lcov mingw-w64 wine
                '''
                sh 'ccache -M 5G'
            }
        }

        stage('Checkout') {
            steps {
                git url: 'https://github.com/T3mps/Arcane.git', branch: 'main'
            }
        }
        
        stage('Static Code Analysis') {
            steps {
                sh 'cppcheck --enable=all --xml src/ 2> cppcheck.xml || true'
                publishXML reportFiles: 'cppcheck.xml', reportName: 'Cppcheck Report'
                sh 'clang-tidy src/**/*.cpp -p build/ > clang-tidy.txt || true'
                archiveArtifacts artifacts: 'clang-tidy.txt', fingerprint: true
            }
        }

        stage('Build for Windows') {
            steps {
                sh './premake5 gmake2 --os=windows'
                sh '''
                CC=${MINGW_PREFIX}-gcc \
                CXX=${MINGW_PREFIX}-g++ \
                AR=${MINGW_PREFIX}-ar \
                RANLIB=${MINGW_PREFIX}-ranlib \
                make config=${BUILD_TYPE}_x86_64 -j$(nproc)
                '''
            }
        }

        stage('Build for Linux') {
            steps {
                sh './premake5 gmake2'
                sh 'make config=${BUILD_TYPE}_x86_64 -j$(nproc)'
            }
        }

        // stage('Unit Tests') {
            // steps {
            //     // Run Linux unit tests
            //     sh './run_unit_tests.sh'
            //     junit 'test-results/**/*.xml'

            //     // Optionally, run Windows unit tests under Wine
            //     sh '''
            //     wine ./bin/${BUILD_TYPE}_x86_64/YourTestExecutable.exe || true
            //     '''

            //     // Collect code coverage if applicable
            //     sh '''
            //     lcov --directory . --capture --output-file coverage.info
            //     lcov --remove coverage.info "/usr/*" --output-file coverage.info
            //     '''
            //     publishCoverage adapters: [lcovAdapter('coverage.info')], sourceFileResolver: sourceFiles('STORE_LAST_BUILD')
            // }
        // }

        stage('Package') {
            steps {
                archiveArtifacts artifacts: 'bin/**', fingerprint: true
                // Optionally, publish to an artifact repository
            }
        }

    }

    post {
        always {
            sh 'ccache -s' // Show ccache statistics
            cleanWs()
        }
        success {
            mail to: 'team@example.com',
                subject: "SUCCESS: ${env.JOB_NAME} Build #${env.BUILD_NUMBER}",
                body: "Good news! The build succeeded. Artifacts and reports are available."
        }
        failure {
            mail to: 'team@example.com',
                subject: "FAILURE: ${env.JOB_NAME} Build #${env.BUILD_NUMBER}",
                body: "The build failed. Please check the console output and address the issues."
        }
    }
}
