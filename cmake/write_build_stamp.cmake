# Writes the build configuration into a stamp file next to a deployed binary.
#
# Invoked from a POST_BUILD step as:
#   cmake -DSTAMP_FILE=<path> -DBUILD_TYPE=$<CONFIG> -P cmake/write_build_stamp.cmake
#
# A -P script rather than `cmake -E echo > file`: custom commands run without a
# shell, so there is no redirection available.
file(WRITE "${STAMP_FILE}" "${BUILD_TYPE}\n")
