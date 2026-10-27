function(nvmetal_unit_test name)
  add_executable(${name} ${name}.c ${ARGN})
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src
                                             ${PROJECT_SOURCE_DIR}/include)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()

nvmetal_unit_test(test_spec)
