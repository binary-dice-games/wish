# cmake/GenerateResource.cmake
# Triggered by add_custom_command to embed the pre-built resources zip
# (WISH_EMBEDDED_ZIP) as a single byte array in embedded_resources.cpp.

file(READ "${WISH_EMBEDDED_ZIP}" hex_content HEX)
string(REGEX REPLACE "(..)" "0x\\1, " CPP_CONTENT "${hex_content}")

configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}" @ONLY)
