# cmake/GenerateResource.cmake
# Triggered by add_custom_command to embed assets via hex arrays

set(CPP_CONTENT "")
set(TABLE_ENTRIES "")

foreach(asset ${ALL_ASSETS})
    # Read the file from its absolute location using WISH_RESOURCE_DIR
    set(full_path "${WISH_RESOURCE_DIR}/${asset}")
    file(READ "${full_path}" hex_content HEX)

    # Format hex digits to C-style array tokens (0xXX, )
    string(REGEX REPLACE "(..)" "0x\\1, " array_content "${hex_content}")

    # Generate a safe variable name (e.g., res_icons_folder_png)
    string(MAKE_C_IDENTIFIER "res_${asset}" sym)

    # Append the raw byte array definition
    string(APPEND CPP_CONTENT "// ${asset}\n")
    string(APPEND CPP_CONTENT "static const unsigned char ${sym}[] = {\n    ${array_content}\n};\n\n")

    # Keep track of the mapping table row for this asset
    string(APPEND TABLE_ENTRIES "    { \"${asset}\", ${sym}, sizeof(${sym}) },\n")
endforeach()

# If no files were found, inject a fallback sentinel entry to prevent compilation errors
if("${TABLE_ENTRIES}" STREQUAL "")
    set(TABLE_ENTRIES "    { nullptr, nullptr, 0 },\n")
    set(RESOURCE_COUNT "0")
else()
    set(RESOURCE_COUNT "sizeof(g_resource_table) / sizeof(g_resource_table[0])")
endif()

# Read your src/resources/embedded_resources.cpp.in template, 
# replace placeholders, and output to src/resources/embedded_resources.cpp
configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}" @ONLY)