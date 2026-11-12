#include "read_file.h"

#include "blockwright/regular_file.h"

namespace bwcli {

std::string readFile(const std::string& path, std::size_t maxSize) {
    using Reason = bw::RegularFileError::Reason;
    try {
        return bw::readRegular(path, maxSize);
    } catch (const bw::RegularFileError& e) {
        switch (e.reason) {
        case Reason::NotRegular:
            throw Failure{path + ": " + e.what()};
        case Reason::TooLarge:
            throw Failure{path + ": input too large"};
        case Reason::Missing:
        case Reason::Unreadable:
            break;
        }
        throw Failure{path + ": " + e.systemMessage()};
    }
}

} // namespace bwcli
