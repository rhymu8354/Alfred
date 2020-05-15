/**
 * @file main.cpp
 *
 * This module holds the main() function, which is the entrypoint
 * to the program.
 */

#include "Service.hpp"

#ifdef _WIN32
#include <crtdbg.h>
#endif /* _WIN32 */

/**
 * This function is the entrypoint of the program.
 *
 * It registers the SIGINT signal to know when the service should be shut down
 * early.
 *
 * @param[in] argc
 *     This is the number of command-line arguments given to the program.
 *
 * @param[in] argv
 *     This is the array of command-line arguments given to the program.
 */
int main(int argc, char* argv[]) {
#ifdef _WIN32
    //_crtBreakAlloc = 18;
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif /* _WIN32 */
    Service service;
    return service.Main(argc, argv);
}
