# GNU style (GCC/Clang) compiler specific settings

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    return()
endif()

add_compile_options(-Wall -Wimplicit -Wshadow
    -Wstrict-prototypes -Wformat=2  -Wformat-security
    -Wstrict-aliasing=2 -Wmissing-format-attribute
    -Wdisabled-optimization -Werror-implicit-function-declaration)

add_compile_options(-Wno-format-zero-length -Wno-format-nonliteral)

# There are lots of instances of union based aliasing in the code
# that rely on the compiler not optimising them away, so disable it
add_compile_options(-fno-strict-aliasing)

# This is necessary to hide all symbols unless explicitly exported
# via the Q_EXPORT macro
#add_compile_options(-fvisibility=hidden)

add_compile_options(-Wno-comment)
# Treat no return type as error
add_compile_options(-Werror=return-type)

execute_process(COMMAND ${CMAKE_C_COMPILER} -v
                OUTPUT_VARIABLE COMPILER_OUTPUT
                ERROR_VARIABLE COMPILER_OUTPUT
                OUTPUT_STRIP_TRAILING_WHITESPACE)

list(APPEND CLIENT_LIBRARIES m)
list(APPEND SERVER_LIBRARIES m)

if(COMPILER_OUTPUT MATCHES "musl|freebsd|openbsd|netbsd")
    list(APPEND CLIENT_LIBRARIES execinfo)
    list(APPEND SERVER_LIBRARIES execinfo)
else()
    # For when using GLIBC versions older than 2.34
    list(APPEND CLIENT_LIBRARIES rt)
    list(APPEND SERVER_LIBRARIES rt)
endif()