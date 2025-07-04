def failedTargets = []

pipeline {
    agent { label 'docker' }
    options { 
        buildDiscarder(logRotator(numToKeepStr: '60', daysToKeepStr: '21'))
        gitLabConnection('GitLabConnectionJenkins')
    }
    parameters {
        booleanParam(name: 'RESULT_TO_SLACK', defaultValue: true, description: 'Should the job result be sent to slack?')
        booleanParam(name: 'CUSTOM_PLATFORM', defaultValue: false, description: 'If true, will use PLATFORM_TO_BUILD. If false, will build for all platforms')
        string(name: 'PLATFORM_TO_BUILD', defaultValue: 'alpine', description: 'Only used if CUSTOM_PLATFORM is true')
        string(name: 'SDK_BRANCH', defaultValue: 'develop', description: 'Define a custom SDK branch.')
    }
    environment {
        VCPKGPATH = "/opt/vcpkg"
        VCPKG_BINARY_SOURCES  = 'clear;x-aws,s3://vcpkg-cache/archives/,readwrite'
        AWS_ACCESS_KEY_ID     = credentials('s4_access_key_id_vcpkg_cache')
        AWS_SECRET_ACCESS_KEY = credentials('s4_secret_access_key_vcpkg_cache')
        AWS_ENDPOINT_URL      = "https://s3.g.s4.mega.io"
    }
    stages {
        stage('Clean previous runs'){
            steps{
                deleteDir()
            }
        }
        stage('Checkout SDK'){
            steps {
                checkout([
                    $class: 'GitSCM', 
                    branches: [[name: "${env.SDK_BRANCH}"]],
                    userRemoteConfigs: [[ url: "${env.GIT_URL_SDK}", credentialsId: "12492eb8-0278-4402-98f0-4412abfb65c1" ]],
                    extensions: [
                        [$class: "UserIdentity",name: "jenkins", email: "jenkins@jenkins"]
                        ]
                ])
                script {
                    sdk_sources_workspace = WORKSPACE
                    build_agent = "${NODE_NAME}"
                }
            }
        }

        stage('Build DSM docker image'){
            steps{
                dir("dockerfile"){
                    sh "docker build -t meganz/dsm-build-env:${env.BUILD_NUMBER} -f ./dms-cross-build.dockerfile ."
                }
            }
        }

        stage ('Build custom platform'){
            when { 
                beforeAgent true
                expression { params.CUSTOM_PLATFORM == true }
            }
            steps {
                echo "Do Build for ${params.PLATFORM_TO_BUILD}"
                dir(sdk_sources_workspace){
                    sh """ 
                        docker run --name dsm-builder-${params.PLATFORM_TO_BUILD}-${env.BUILD_NUMBER} --rm \
                            -v ${sdk_sources_workspace}:/mega/sdk \
                            -v ${VCPKGPATH}:/mega/vcpkg \
                            -e PLATFORM=${params.PLATFORM_TO_BUILD} \
                            -e VCPKG_BINARY_SOURCES \
                            -e AWS_ACCESS_KEY_ID \
                            -e AWS_SECRET_ACCESS_KEY \
                            -e AWS_ENDPOINT_URL \
                            meganz/dsm-build-env:${env.BUILD_NUMBER}
                    """
                }
            }
            post{
                aborted {
                    sh "docker kill android-builder-${params.PLATFORM_TO_BUILD}-${env.BUILD_NUMBER}"
                    script {
                        failedTargets.add("${params.PLATFORM_TO_BUILD}")
                    }
                }
                failure {
                    script {
                        failedTargets.add("${params.PLATFORM_TO_BUILD}")
                    }
                }
            }           
        }

        stage ('Build all platforms'){
            when {
                beforeAgent true
                expression { params.CUSTOM_PLATFORM == false }
            }
            matrix {
                axes {
                    axis { 
                        name 'PLATFORM';
                        values 'alpine', 'alpine4k', 'apollolake', 'armada37xx', 'armada38x',
                               'avoton','braswell', 'broadwell', 'broadwellnk', 'broadwellnkv2',
                               'broadwellntbap', 'bromolow', 'denverton', 'epyc7002',
                               'geminilake', 'grantley', 'kvmx64', 'monaco',
                               'purley', 'r1000', 'rtd1296', 'rtd1619b', 'v1000'
                    }
                }
                stages {
                    stage('Build') {
                        agent { label "${build_agent}" }
                        steps {
                            echo "Do Build for DSM - ${PLATFORM}"
                            dir(sdk_sources_workspace){
                                sh """ 
                                    docker run --name dsm-builder-${PLATFORM}-${env.BUILD_NUMBER} --rm \
                                        -v ${sdk_sources_workspace}:/mega/sdk \
                                        -v ${VCPKGPATH}:/mega/vcpkg \
                                        -e VCPKG_BINARY_SOURCES \
                                        -e AWS_ACCESS_KEY_ID \
                                        -e AWS_SECRET_ACCESS_KEY \
                                        -e AWS_ENDPOINT_URL \
                                        -e PLATFORM=${PLATFORM} meganz/dsm-build-env:${env.BUILD_NUMBER}
                                """
                            }
                        }
                        post{
                            aborted {
                                sh "docker kill android-builder-${PLATFORM}-${env.BUILD_NUMBER}"
                                script {
                                    failedTargets.add("${PLATFORM}")
                                }
                            }
                            failure {
                                script {
                                    failedTargets.add("${PLATFORM}")
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    post {
        always {
            sh "docker image rm meganz/dsm-build-env:${env.BUILD_NUMBER}"
            script {
                if (params.RESULT_TO_SLACK) {
                    sdk_commit = sh(script: "git -C ${sdk_sources_workspace} rev-parse HEAD", returnStdout: true).trim()
                    messageStatus = currentBuild.currentResult
                    messageColor = messageStatus == 'SUCCESS'? "#00FF00": "#FF0000" //green or red
                    message = """
                        *DSM* <${BUILD_URL}|Build result>: '${messageStatus}'.
                        SDK branch: `${SDK_BRANCH}`
                        SDK commit: `${sdk_commit}`
                    """.stripIndent()
                    
                    if (failedTargets.size() > 0) {
                        message += "\nFailed targets: ${failedTargets.join(', ')}"
                    }
                    withCredentials([string(credentialsId: 'slack_webhook_sdk_report', variable: 'SLACK_WEBHOOK_URL')]) {
                        sh """
                            curl -X POST -H 'Content-type: application/json' --data '
                                {
                                "attachments": [
                                    {
                                        "color": "${messageColor}",
                                        "blocks": [
                                        {
                                            "type": "section",
                                            "text": {
                                                    "type": "mrkdwn",
                                                    "text": "${message}"
                                            }
                                        }
                                        ]
                                    }
                                    ]
                                }' \${SLACK_WEBHOOK_URL}
                        """
                    }
                }
            }
            deleteDir() /* clean up our workspace */
        }
    }
}
