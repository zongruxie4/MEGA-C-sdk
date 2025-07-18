add_executable(test_integration)

target_sources(test_integration
    PRIVATE
    env_var_accounts.h
    SdkTest_test.h
    SdkTestNodesSetUp.h
    SdkTestSyncNodesOperations.h
    SdkTestSyncPrevalidation.h
    test.h
    integration_test_utils.h
    mock_listeners.h

    env_var_accounts.cpp
    main.cpp
    SdkTest_test.cpp
    SdkTestNodesSetUp.cpp
    SdkTestSyncNodesOperations.cpp
    SdkTestFilter_test.cpp
    SdkTestCreateAccount_test.cpp
    SdkTestTransferStats_test.cpp
    sdk_test_user_alerts.cpp
    sdk_test_user_attributes.cpp
    SdkTestSyncRootOperations_test.cpp
    SdkTestSyncUploadThrottling_test.cpp
    SdkTestChangeLocalSyncRoot_test.cpp
    SdkTestSyncPrevalidation.cpp
    SdkTestSyncPrevalidation_test.cpp
    Sync_test.cpp
    one_question_survey_test.cpp
    share_test.cpp
    integration_test_utils.cpp
    DisableBackupSync_test.cpp
    SdkTestTransferMaxSpeeds_test.cpp
    sdk_test_file_path.cpp
    sdk_test_http_server.cpp
    sdk_test_node_tags.cpp
    sdk_test_node_tags.h
    sdk_test_network.cpp
    SdkTestDeviceCenter_test.cpp
    SdkTestSyncLocalOperations_test.cpp
    SdkTestSyncNodeAttribute_test.cpp
    backup_sync_operations_test.cpp

    passwordManager/SdkTestPasswordManager.cpp
    passwordManager/SdkTestPasswordManager.h
    passwordManager/SdkTestPasswordManagerBaseNode_test.cpp
    passwordManager/SdkTestPasswordManagerImport_test.cpp
    passwordManager/SdkTestPasswordManagerPassFolderCRUD_test.cpp
    passwordManager/SdkTestPasswordManagerPassNodeCRUD_test.cpp
    passwordManager/SdkTestPasswordManagerCreditCardNodeCRUD_test.cpp
)

target_sources_conditional(test_integration
    FLAG ENABLE_ISOLATED_GFX
    PRIVATE
    IsolatedGfx_test.cpp
)

# Link with SDKlib
target_link_libraries(test_integration PRIVATE MEGA::SDKlib)

# Link with the common and tools interface libraries for the tests.
target_link_libraries(test_integration
    PRIVATE
    MEGA::test_tools
    MEGA::test_common
)

target_include_directories(test_integration
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Adjust compilation flags for warnings and errors
target_platform_compile_options(
    TARGET test_integration
    WINDOWS /bigobj /we4800 # Implicit conversion from 'type' to bool. Possible information loss
    UNIX $<$<CONFIG:Debug>:-ggdb3> -Wall -Wextra -Wconversion
)

if(ENABLE_SDKLIB_WERROR)
    target_platform_compile_options(
        TARGET test_integration
        WINDOWS /WX
        UNIX  $<$<CONFIG:Debug>: -Werror>
    )
endif()

# Integration tests require the following files to work
file(GLOB TESTING_AUX_FILES "test-data/*")
add_custom_command(
    TARGET test_integration POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${TESTING_AUX_FILES} $<TARGET_FILE_DIR:test_integration>
    COMMENT "Copying test files for integration tests."
)

if(TARGET gfxworker)
    add_custom_command(
        TARGET test_integration POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gfxworker> $<TARGET_FILE_DIR:test_integration>
        COMMENT "Copying gfxworker for integration tests."
    )
endif()
