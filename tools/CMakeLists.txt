foreach(tool nvmetal-identify)
  add_executable(${tool} ${tool}.c)
  target_link_libraries(${tool} PRIVATE nvmetal)
endforeach()
