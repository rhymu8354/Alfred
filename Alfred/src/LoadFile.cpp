/**
 * @file LoadFile.cpp
 *
 * This module contains the implementation of the LoadFile function.
 */

#include "LoadFile.hpp"

#include <stdint.h>
#include <SystemAbstractions/File.hpp>
#include <vector>

bool LoadFile(
    const std::string& filePath,
    const std::string& fileDescription,
    const SystemAbstractions::DiagnosticsSender& diagnosticsSender,
    std::string& fileContents
) {
    SystemAbstractions::File file(filePath);
    if (
        !file.IsDirectory()
        && file.OpenReadOnly()
    ) {
        std::vector< uint8_t > fileContentsAsVector(file.GetSize());
        if (file.Read(fileContentsAsVector) != fileContentsAsVector.size()) {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "Unable to read %s file '%s'",
                fileDescription.c_str(),
                filePath.c_str()
            );
            return false;
        }
        (void)fileContents.assign(
            (const char*)fileContentsAsVector.data(),
            fileContentsAsVector.size()
        );
    } else {
        diagnosticsSender.SendDiagnosticInformationFormatted(
            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
            "Unable to open %s file '%s'",
            fileDescription.c_str(),
            filePath.c_str()
        );
        return false;
    }
    return true;
}
