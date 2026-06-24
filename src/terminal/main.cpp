#include "fox/ai_kernel.hpp"
#include "fox/command_router.hpp"

#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>

namespace {

void printBanner()
{
    std::cout << "FOX-terminal 0.1.0\n";
    std::cout << "Port OS first setup command: /port ans-install-fd\n";
    std::cout << "Type 'help' for commands. Type 'exit' to quit.\n\n";
}

} // namespace

int main()
{
    fox::AIKernel kernel;
    fox::CommandRouter router;

    printBanner();

    const auto bootResponse = kernel.boot();
    std::cout << bootResponse.message << "\n\n";

    std::string input;
    while (kernel.isRunning()) {
        std::cout << "fox> ";

        if (!std::getline(std::cin, input)) {
            std::cout << '\n';
            break;
        }

        const auto command = router.parse(input);
        const auto response = kernel.handleCommand(command);

        if (!response.message.empty()) {
            if (response.message.rfind("CONFIRM_REQUIRED: ", 0) == 0) {
                std::string filename = response.message.substr(18);
                std::cout << "Are you sure you want to delete '" << filename << "'? (y/N): ";
                std::string confirm;
                if (std::getline(std::cin, confirm)) {
                    std::transform(confirm.begin(), confirm.end(), confirm.begin(), ::tolower);
                    if (confirm == "y" || confirm == "yes") {
                        fox::Command confirmCommand = command;
                        confirmCommand.arg2 = "yes";
                        const auto confirmResponse = kernel.handleCommand(confirmCommand);
                        if (!confirmResponse.message.empty()) {
                            std::cout << confirmResponse.message << "\n";
                        }
                        if (!confirmResponse.ok) {
                            std::cout << "Command failed.\n";
                        }
                    } else {
                        std::cout << "Deletion cancelled.\n";
                    }
                }
            } else {
                std::cout << response.message << "\n";
            }
        }

        if (!response.ok && response.message.rfind("CONFIRM_REQUIRED: ", 0) != 0) {
            std::cout << "Command failed.\n";
        }

        std::cout << '\n';
    }

    return 0;
}
