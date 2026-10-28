add_executable(nvmetal-identify nvmetal-identify.c)
target_link_libraries(nvmetal-identify PRIVATE nvmetal)

add_executable(nvmetal-verify nvmetal-verify.c crc32c.c)
target_link_libraries(nvmetal-verify PRIVATE nvmetal)
