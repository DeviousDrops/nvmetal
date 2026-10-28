# Programs that need a real (emulated) controller bound to vfio-pci. They are
# driven by scripts/test_guest.sh inside the VM, not by ctest.
function(nvmetal_guest_test name)
  add_executable(${name} ${name}.c)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(${name} PRIVATE nvmetal)
endfunction()

nvmetal_guest_test(vfio_probe)
nvmetal_guest_test(io_smoke)
