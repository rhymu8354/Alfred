#pragma once

/**
 * @file ApiHttp.hpp
 *
 * This module declares functions which implement an Application Programming
 * Interface (API) to the service via Hypertext Transfer Protocol (HTTP).
 */

#include "Store.hpp"

#include <Http/Server.hpp>
#include <memory>

namespace ApiHttp {

    void RegisterResources(
        const std::shared_ptr< Store >& store,
        Http::Server& httpServer
    );

}
