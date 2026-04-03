if (NOT TARGET OpenSSL::Crypto)
    # The NDK does not include OpenSSL, download it
    set(OPENSSL_VERSION "3.6.0")
    set(OPENSSL_SHA256 b6a5f44b7eb69e3fa35dbf15524405b44837a481d43d81daddde3ff21fcbb8e9)
    file(GLOB ANDROID_NDK_LLVM_BIN_DIRS LIST_DIRECTORIES true "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/*/bin")
    list(LENGTH ANDROID_NDK_LLVM_BIN_DIRS ANDROID_NDK_LLVM_BIN_DIR_COUNT)
    if (ANDROID_NDK_LLVM_BIN_DIR_COUNT EQUAL 0)
        message(FATAL_ERROR "Could not find Android NDK LLVM toolchain bin directory under ${CMAKE_ANDROID_NDK}")
    endif()
    list(GET ANDROID_NDK_LLVM_BIN_DIRS 0 ANDROID_NDK_LLVM_BIN_DIR)
    set(OPENSSL_BUILD_ENV_PATH "${ANDROID_NDK_LLVM_BIN_DIR}:/usr/bin:/bin:/usr/sbin:/sbin:$ENV{PATH}")
    if (CMAKE_BUILD_PARALLEL_LEVEL)
        set(OPENSSL_MAKE_JOBS "${CMAKE_BUILD_PARALLEL_LEVEL}")
    else()
        set(OPENSSL_MAKE_JOBS "4")
    endif()

    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl-src)
        if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz)
            if (EXISTS ${CMAKE_SOURCE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz)
                file(CREATE_LINK ${CMAKE_SOURCE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz SYMBOLIC)
            else()
                file(DOWNLOAD https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz
                     ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz
                     EXPECTED_HASH SHA256=${OPENSSL_SHA256})
            endif()
        endif()

        file(ARCHIVE_EXTRACT INPUT ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz DESTINATION ${FETCHCONTENT_BASE_DIR})
        file(RENAME ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION} ${FETCHCONTENT_BASE_DIR}/openssl-src)
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK} PATH=${OPENSSL_BUILD_ENV_PATH} ./Configure
            android-arm64
            shared
            no-apps
            no-tests
            --prefix=${FETCHCONTENT_BASE_DIR}/openssl
            --openssldir=${FETCHCONTENT_BASE_DIR}/openssl
        WORKING_DIRECTORY ${FETCHCONTENT_BASE_DIR}/openssl-src
        OUTPUT_FILE ${CMAKE_BINARY_DIR}/openssl-config-out
        ERROR_FILE ${CMAKE_BINARY_DIR}/openssl-config-err
        RESULT_VARIABLE OPENSSL_CONFIG_RESULT
    )
    if (NOT OPENSSL_CONFIG_RESULT EQUAL 0)
        message(FATAL_ERROR "OpenSSL configure failed, see ${CMAKE_BINARY_DIR}/openssl-config-out and ${CMAKE_BINARY_DIR}/openssl-config-err")
    endif()

    if (NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl/lib/libcrypto.so)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK} PATH=${OPENSSL_BUILD_ENV_PATH} make -j${OPENSSL_MAKE_JOBS}
            WORKING_DIRECTORY ${FETCHCONTENT_BASE_DIR}/openssl-src
            OUTPUT_FILE ${CMAKE_BINARY_DIR}/openssl-make-out
            ERROR_FILE ${CMAKE_BINARY_DIR}/openssl-make-err
            RESULT_VARIABLE OPENSSL_MAKE_RESULT
        )
        if (NOT OPENSSL_MAKE_RESULT EQUAL 0)
            message(FATAL_ERROR "OpenSSL make failed, see ${CMAKE_BINARY_DIR}/openssl-make-out and ${CMAKE_BINARY_DIR}/openssl-make-err")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK} PATH=${OPENSSL_BUILD_ENV_PATH} make install_sw
            WORKING_DIRECTORY ${FETCHCONTENT_BASE_DIR}/openssl-src
            OUTPUT_FILE ${CMAKE_BINARY_DIR}/openssl-install-out
            ERROR_FILE ${CMAKE_BINARY_DIR}/openssl-install-err
            RESULT_VARIABLE OPENSSL_INSTALL_RESULT
        )
        if (NOT OPENSSL_INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "OpenSSL install failed, see ${CMAKE_BINARY_DIR}/openssl-install-out and ${CMAKE_BINARY_DIR}/openssl-install-err")
        endif()
    endif()

    add_library(OpenSSL::Crypto STATIC IMPORTED)
    set_property(TARGET OpenSSL::Crypto PROPERTY IMPORTED_LOCATION ${FETCHCONTENT_BASE_DIR}/openssl/lib/libcrypto.so)
    target_include_directories(OpenSSL::Crypto INTERFACE ${FETCHCONTENT_BASE_DIR}/openssl/include)

    add_library(OpenSSL::SSL STATIC IMPORTED)
    set_property(TARGET OpenSSL::SSL PROPERTY IMPORTED_LOCATION ${FETCHCONTENT_BASE_DIR}/openssl/lib/libssl.so)
    target_include_directories(OpenSSL::SSL INTERFACE ${FETCHCONTENT_BASE_DIR}/openssl/include)
endif()
