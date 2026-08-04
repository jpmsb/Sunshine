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

# Fork version stamp: YYYY.MMDD.HHMMSS-jpmsb (configure timestamp).
# Used for local and CI builds so this fork always exposes a consistent CalVer.
string(TIMESTAMP PROJECT_VERSION_YEAR "%Y")
string(TIMESTAMP PROJECT_VERSION_MONTH_DAY "%m%d")
string(TIMESTAMP PROJECT_VERSION_HMS "%H%M%S")
set(PROJECT_VERSION_CORE
        "${PROJECT_VERSION_YEAR}.${PROJECT_VERSION_MONTH_DAY}.${PROJECT_VERSION_HMS}")
set(PROJECT_VERSION "${PROJECT_VERSION_CORE}-jpmsb")
# CPack/numeric fields omit the fork suffix (hyphens break some package version schemas).
set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION_CORE})

if((DEFINED ENV{BRANCH}) AND (DEFINED ENV{BUILD_VERSION}) AND (NOT "$ENV{BUILD_VERSION}" STREQUAL ""))  # cmake-lint: disable=W0106
    message("CI branch '$ENV{BRANCH}' provided BUILD_VERSION='$ENV{BUILD_VERSION}'; using fork stamp ${PROJECT_VERSION}")
else()
    message("Sunshine build version: ${PROJECT_VERSION}")
endif()

# set date variables
set(PROJECT_YEAR "1990")
set(PROJECT_MONTH "01")
set(PROJECT_DAY "01")

# Extract year, month, and day from the numeric core (YYYY.MMDD.HHMMSS)
# Note: Cmake doesn't support "{}" regex syntax
if(PROJECT_VERSION_CORE MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9][0-9][0-9][0-9]?)\\.([0-9]+)$")
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

# Parse PROJECT_VERSION_CORE to extract major, minor, and patch components
if(PROJECT_VERSION_CORE MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
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

# llvm-rc (Clang ARM64) parses RC integers with a leading zero as octal.
# CalVer MMDD (e.g. 0802 for August 2) is invalid octal because of digit 8.
# Keep zero-padded strings in PROJECT_VERSION*; expose decimal ints for VERSIONINFO.
if(NOT DEFINED PROJECT_VERSION_MAJOR)
    set(PROJECT_VERSION_MAJOR 0)
endif()
if(NOT DEFINED PROJECT_VERSION_MINOR)
    set(PROJECT_VERSION_MINOR 0)
endif()
math(EXPR RC_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
math(EXPR RC_VERSION_MINOR "${PROJECT_VERSION_MINOR}")

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
message("RC_VERSION_MAJOR: ${RC_VERSION_MAJOR}")
message("RC_VERSION_MINOR: ${RC_VERSION_MINOR}")
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
