idf_component_register(SRCS "packfs.c" "fileops.c" "statops.c" "dirops.c" "process.c" "stream.c" "imagefs.c" "imagefsops.c" "imagefsdfu.c" "lzoops.c" "minilzo.c"
    INCLUDE_DIRS "." "include/"
    REQUIRES app_update mbedtls)
