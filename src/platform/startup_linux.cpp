#include "platform.h"

#include <sys/utsname.h>

#include <fstream>
#include <string>

void PlatformStartup() {
    // Nothing to do: everything FastPlay uses is linked in or is the system's.
}

// The distribution's name from /etc/os-release ("Fedora Linux 45 (Workstation
// Edition)"), and the machine.
std::string GetSystemDescription() {
    std::string name = "Linux";
    std::ifstream release("/etc/os-release");
    std::string line;
    while (std::getline(release, line)) {
        if (line.compare(0, 12, "PRETTY_NAME=") != 0) continue;
        std::string value = line.substr(12);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty()) name = value;
        break;
    }
    struct utsname system = {};
    if (uname(&system) == 0) {
        name += "; ";
        name += system.machine;
    }
    return name;
}
