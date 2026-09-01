cmake_minimum_required(VERSION 3.21)
project(n3ds_preprocess C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

add_executable(n3ds-preprocess
    main.c
    stb_impl.c
    ../../src/binary_reader.c
    ../../src/data_win.c
    ../../src/gl/image_decoder.c
    ../../vendor/bzip2/huffman.c
    ../../vendor/bzip2/crctable.c
    ../../vendor/bzip2/randtable.c
    ../../vendor/bzip2/decompress.c
    ../../vendor/bzip2/bzlib.c
    ../../vendor/bzip2/bz_internal_error.c
)

target_include_directories(n3ds-preprocess PRIVATE
    ../../src
    ../../vendor
    ../../vendor/bzip2
    ../../vendor/stb/ds
    ../../vendor/stb/image
)

target_compile_definitions(n3ds-preprocess PRIVATE
    IMAGE_DECODER_HAS_BZ2=1
    N3DS_PREPROCESS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    N3DS_PREPROCESS_TEXTURE_OVERRIDE_DIR="${CMAKE_SOURCE_DIR}/resources/3ds/textureOverrides"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(n3ds-preprocess PRIVATE m)
endif()
