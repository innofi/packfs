idf_component_register(SRCS "packfs.c" "fileops.c" "statops.c" "dirops.c" "lzoops.c" "minilzo.c"
    INCLUDE_DIRS "." "include/"
    REQUIRES app_update mbedtls)

# "process.c" "stream.c" "imagefs.c" "imagefsops.c" "imagefsdfu.c"