#include "ui/CliMenu.h"

#include <iostream>

int CliMenu::run() {
    std::cout << "Press 'q' to quit." << std::endl;
    char c = 0;
    while (std::cin >> c) {
        if (c == 'q' || c == 'Q') {
            break;
        }
    }
    return 0;
}

