# CMake generated Testfile for 
# Source directory: /mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/test
# Build directory: /mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/build-1.001.1.12/test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[relay_rover_compare_e2e]=] "bash" "/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/test/cases/t_relay_rover_compare.sh")
set_tests_properties([=[relay_rover_compare_e2e]=] PROPERTIES  ENVIRONMENT "NTRIPCASTER_BIN=/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/build-1.001.1.12/src/ntripcaster;RELAY_BIN=/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/build-1.001.1.12/test/tools/ntrip_source_relay;ROVER_BIN=/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/build-1.001.1.12/test/tools/ntrip_rover_client;COMPARE_BIN=/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/build-1.001.1.12/test/tools/ntrip_capture_compare;MK_RTCM_PY=/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/test/helpers/mk_rtcm.py" TIMEOUT "60" _BACKTRACE_TRIPLES "/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/test/CMakeLists.txt;8;add_test;/mnt/c/Users/cesar/Desktop/Claude Cowork/ntripcaster/wsl-project-tested/test/CMakeLists.txt;0;")
subdirs("tools")
