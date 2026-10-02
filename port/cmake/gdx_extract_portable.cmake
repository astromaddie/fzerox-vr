# Injected into the gdx-extract (Torch) ExternalProject via CMAKE_PROJECT_INCLUDE, so it runs after
# every project() call there. Torch assigns CMAKE_CXX_FLAGS outright, which would discard flags
# passed on the command line; directory-level compile options survive that. See the comment above
# ExternalProject_Add(gdx_extract_ext) in port/CMakeLists.txt for why these two flags are required
# for the golden archive hash to reproduce on ARM64.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsigned-char -ffp-contract=off)
endif()
