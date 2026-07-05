# Set build variables if env variables are defined
# These are used in configured files such as manifests for different packages
if(DEFINED ENV{BRANCH})
    set(GITHUB_BRANCH $ENV{BRANCH})
endif()
if(DEFINED ENV{BUILD_VERSION})  # cmake-lint: disable=W0106
    set(BUILD_VERSION $ENV{BUILD_VERSION})
endif()
if(DEFINED ENV{CLONE_URL})
    set(GITHUB_CLONE_URL $ENV{CLONE_URL})
endif()
if(DEFINED ENV{COMMIT})
    set(GITHUB_COMMIT $ENV{COMMIT})
endif()
if(DEFINED ENV{TAG})
    set(GITHUB_TAG $ENV{TAG})
endif()

# Check if env vars are defined before attempting to access them, variables will be defined even if blank
if((DEFINED ENV{BRANCH}) AND (DEFINED ENV{BUILD_VERSION}))  # cmake-lint: disable=W0106
    if((DEFINED ENV{BRANCH}) AND (NOT $ENV{BUILD_VERSION} STREQUAL ""))
        # If BRANCH is defined and BUILD_VERSION is not empty, then we are building from CI
        # If BRANCH is master we are building a push/release build
        MESSAGE("Got from CI '$ENV{BRANCH}' branch and version '$ENV{BUILD_VERSION}'")
        set(PROJECT_VERSION $ENV{BUILD_VERSION})
        string(REGEX REPLACE "^v" "" PROJECT_VERSION ${PROJECT_VERSION})  # remove the v prefix if it exists
        set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})  # cpack will use this to set the binary versions
    endif()
else()
    # Local builds use AAAAMMDDHHmmss-jpmsb based on the configure timestamp.
    string(TIMESTAMP BUILD_TIMESTAMP "%Y%m%d%H%M%S")
    set(PROJECT_VERSION "${BUILD_TIMESTAMP}-jpmsb")
    set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})

    find_package(Git)
    if(GIT_EXECUTABLE)
        execute_process(
                COMMAND ${GIT_EXECUTABLE} diff --quiet --exit-code
                RESULT_VARIABLE GIT_IS_DIRTY
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(GIT_IS_DIRTY)
            set(PROJECT_VERSION "${PROJECT_VERSION}-dirty")
            set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})
            MESSAGE("Git tree is dirty!")
        endif()
    endif()

    MESSAGE("Sunshine local build version: ${PROJECT_VERSION}")
endif()

# set date variables
set(PROJECT_YEAR "1990")
set(PROJECT_MONTH "01")
set(PROJECT_DAY "01")

# Extract year, month, and day (do this AFTER version parsing)
# Note: Cmake doesn't support "{}" regex syntax
if(PROJECT_VERSION MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9][0-9][0-9][0-9]?)\\.([0-9]+)$")
    message("Extracting year and month/day from PROJECT_VERSION: ${PROJECT_VERSION}")
    # First capture group is the year
    set(PROJECT_YEAR "${CMAKE_MATCH_1}")

    # Second capture group contains month and day
    set(MONTH_DAY "${CMAKE_MATCH_2}")

    # Extract month (first 1-2 digits) and day (last 2 digits)
    string(LENGTH "${MONTH_DAY}" MONTH_DAY_LENGTH)
    if(MONTH_DAY_LENGTH EQUAL 3)
        # Format: MDD (e.g., 703 = month 7, day 03)
        string(SUBSTRING "${MONTH_DAY}" 0 1 PROJECT_MONTH)
        string(SUBSTRING "${MONTH_DAY}" 1 2 PROJECT_DAY)
    elseif(MONTH_DAY_LENGTH EQUAL 4)
        # Format: MMDD (e.g., 1203 = month 12, day 03)
        string(SUBSTRING "${MONTH_DAY}" 0 2 PROJECT_MONTH)
        string(SUBSTRING "${MONTH_DAY}" 2 2 PROJECT_DAY)
    endif()

    # Ensure month is two digits
    if(PROJECT_MONTH LESS 10 AND NOT PROJECT_MONTH MATCHES "^0")
        set(PROJECT_MONTH "0${PROJECT_MONTH}")
    endif()
    # Ensure day is two digits
    if(PROJECT_DAY LESS 10 AND NOT PROJECT_DAY MATCHES "^0")
        set(PROJECT_DAY "0${PROJECT_DAY}")
    endif()
endif()

# jpmsb timestamp format: AAAAMMDDHHmmss-jpmsb
set(VERSION_STAMP "${PROJECT_VERSION}")
string(REGEX REPLACE "-dirty$" "" VERSION_STAMP "${VERSION_STAMP}")

if(VERSION_STAMP MATCHES "^([0-9][0-9][0-9][0-9])([0-9][0-9])([0-9][0-9])([0-9][0-9])([0-9][0-9])([0-9][0-9])-jpmsb$")
    message("Extracting components from jpmsb PROJECT_VERSION: ${PROJECT_VERSION}")
    set(PROJECT_YEAR "${CMAKE_MATCH_1}")
    set(PROJECT_MONTH "${CMAKE_MATCH_2}")
    set(PROJECT_DAY "${CMAKE_MATCH_3}")
    set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(CMAKE_PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}${CMAKE_MATCH_3}")
    set(CMAKE_PROJECT_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
    math(EXPR VERSION_HH "${CMAKE_MATCH_4}")
    math(EXPR VERSION_MM "${CMAKE_MATCH_5}")
    math(EXPR VERSION_SS "${CMAKE_MATCH_6}")
    math(EXPR PROJECT_VERSION_PATCH "${VERSION_HH} * 10000 + ${VERSION_MM} * 100 + ${VERSION_SS}")
    set(CMAKE_PROJECT_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
endif()

# Parse PROJECT_VERSION to extract major, minor, and patch components
if(PROJECT_VERSION MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(CMAKE_PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")

    set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(CMAKE_PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")

    set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(CMAKE_PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
endif()

# Split PROJECT_VERSION_PATCH for RC file (Windows VERSIONINFO requires values <= 65535)
# PROJECT_VERSION_PATCH can be 0-245959, so we split it into two parts:
# - Last 2 digits for RC_VERSION_REVISION
# - Leading digits for RC_VERSION_BUILD (0 if original is <= 99)
if(NOT DEFINED PROJECT_VERSION_PATCH)
    set(PROJECT_VERSION_PATCH 0)
endif()
math(EXPR RC_VERSION_BUILD "${PROJECT_VERSION_PATCH} / 100")
math(EXPR RC_VERSION_REVISION "${PROJECT_VERSION_PATCH} % 100")

message("PROJECT_FQDN: ${PROJECT_FQDN}")
message("PROJECT_NAME: ${PROJECT_NAME}")
message("PROJECT_VERSION: ${PROJECT_VERSION}")
message("PROJECT_VERSION_MAJOR: ${PROJECT_VERSION_MAJOR}")
message("PROJECT_VERSION_MINOR: ${PROJECT_VERSION_MINOR}")
message("PROJECT_VERSION_PATCH: ${PROJECT_VERSION_PATCH}")
message("CMAKE_PROJECT_VERSION: ${CMAKE_PROJECT_VERSION}")
message("CMAKE_PROJECT_VERSION_MAJOR: ${CMAKE_PROJECT_VERSION_MAJOR}")
message("CMAKE_PROJECT_VERSION_MINOR: ${CMAKE_PROJECT_VERSION_MINOR}")
message("CMAKE_PROJECT_VERSION_PATCH: ${CMAKE_PROJECT_VERSION_PATCH}")
message("RC_VERSION_BUILD: ${RC_VERSION_BUILD}")
message("RC_VERSION_REVISION: ${RC_VERSION_REVISION}")
message("PROJECT_YEAR: ${PROJECT_YEAR}")
message("PROJECT_MONTH: ${PROJECT_MONTH}")
message("PROJECT_DAY: ${PROJECT_DAY}")

list(APPEND SUNSHINE_DEFINITIONS PROJECT_FQDN="${PROJECT_FQDN}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_NAME="${PROJECT_NAME}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION="${PROJECT_VERSION}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_MAJOR="${PROJECT_VERSION_MAJOR}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_MINOR="${PROJECT_VERSION_MINOR}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_PATCH="${PROJECT_VERSION_PATCH}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_COMMIT="${GITHUB_COMMIT}")
