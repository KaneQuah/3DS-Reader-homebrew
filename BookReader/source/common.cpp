#include "common.h"
#include <algorithm>
#include <cctype>

DocType docTypeFromPath(const std::string& path) {
    auto ext = path.substr(path.rfind('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "epub") return DocType::EPUB;
    if (ext == "txt")  return DocType::TXT;
    if (ext == "pdf")  return DocType::PDF;
    return DocType::Unknown;
}
