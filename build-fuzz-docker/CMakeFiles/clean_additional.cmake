# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "Fuzz/CMakeFiles/fuzz_content_stream_autogen.dir/AutogenUsed.txt"
  "Fuzz/CMakeFiles/fuzz_content_stream_autogen.dir/ParseCache.txt"
  "Fuzz/CMakeFiles/fuzz_images_autogen.dir/AutogenUsed.txt"
  "Fuzz/CMakeFiles/fuzz_images_autogen.dir/ParseCache.txt"
  "Fuzz/CMakeFiles/fuzz_pdf_parser_autogen.dir/AutogenUsed.txt"
  "Fuzz/CMakeFiles/fuzz_pdf_parser_autogen.dir/ParseCache.txt"
  "Fuzz/CMakeFiles/fuzz_stream_filters_autogen.dir/AutogenUsed.txt"
  "Fuzz/CMakeFiles/fuzz_stream_filters_autogen.dir/ParseCache.txt"
  "Fuzz/fuzz_content_stream_autogen"
  "Fuzz/fuzz_images_autogen"
  "Fuzz/fuzz_pdf_parser_autogen"
  "Fuzz/fuzz_stream_filters_autogen"
  "Pdf4QtLibCore/CMakeFiles/Pdf4QtLibCore_autogen.dir/AutogenUsed.txt"
  "Pdf4QtLibCore/CMakeFiles/Pdf4QtLibCore_autogen.dir/ParseCache.txt"
  "Pdf4QtLibCore/Pdf4QtLibCore_autogen"
  )
endif()
