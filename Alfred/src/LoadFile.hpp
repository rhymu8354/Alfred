#pragma once

/**
 * @file LoadFile.hpp
 *
 * This module declares the LoadFile function.
 */

#include <string>
#include <SystemAbstractions/DiagnosticsSender.hpp>

/**
 * This function loads the contents of the file with the given path
 * into the given string.
 *
 * @param[in] filePath
 *     This is the path of the file to load.
 *
 * @param[in] fileDescription
 *     This is a description of the file being loaded, used in any
 *     diagnostic messages published by the function.
 *
 * @param[in] diagnosticsSender
 *     This is the object to use to publish any diagnostic messages.
 *
 * @param[out] fileContents
 *     This is where to store the file's contents.
 *
 * @return
 *     An indication of whether or not the function succeeded is returned.
 */
bool LoadFile(
    const std::string& filePath,
    const std::string& fileDescription,
    const SystemAbstractions::DiagnosticsSender& diagnosticsSender,
    std::string& fileContents
);
