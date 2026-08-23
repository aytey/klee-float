# Tries to find an install of the Bitwuzla library and header files
#
# Once done this will define
#  Bitwuzla_FOUND - BOOL: System has the Bitwuzla library installed
#  Bitwuzla_INCLUDE_DIRS - LIST: The Bitwuzla include directories
#  Bitwuzla_LIBRARIES - LIST: The libraries needed to use Bitwuzla
include(FindPackageHandleStandardArgs)

# Try to find the library
find_library(Bitwuzla_LIBRARIES
  NAMES bitwuzla
  DOC "Bitwuzla libraries"
)
if (Bitwuzla_LIBRARIES)
  message(STATUS "Found Bitwuzla libraries: \"${Bitwuzla_LIBRARIES}\"")
else()
  message(STATUS "Could not find Bitwuzla libraries")
endif()

# Try to find the headers. The C API lives at <bitwuzla/c/bitwuzla.h>, so this
# looks for the directory that prefix is relative to.
find_path(Bitwuzla_INCLUDE_DIRS
  NAMES bitwuzla/c/bitwuzla.h
  DOC "Bitwuzla C header"
)
if (Bitwuzla_INCLUDE_DIRS)
  message(STATUS "Found Bitwuzla include path: \"${Bitwuzla_INCLUDE_DIRS}\"")
else()
  message(STATUS "Could not find Bitwuzla include path")
endif()

# Handle QUIET and REQUIRED and check the necessary variables were set and if so
# set ``Bitwuzla_FOUND``
find_package_handle_standard_args(Bitwuzla DEFAULT_MSG
  Bitwuzla_INCLUDE_DIRS Bitwuzla_LIBRARIES)
