#pragma once

/**
 * @file ApiWs.hpp
 *
 * This module declares the ApiWs class which manages an Application
 * Programming Interface (API) to the service via WebSockets (WS).
 */

#include "Store.hpp"

#include <Http/Server.hpp>
#include <Json/Value.hpp>
#include <memory>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <Timekeeping/Clock.hpp>

class ApiWs {
    // Lifecycle
public:
    ~ApiWs() noexcept;
    ApiWs(const ApiWs&) = delete;
    ApiWs(ApiWs&&) noexcept;
    ApiWs& operator=(const ApiWs&) = delete;
    ApiWs& operator=(ApiWs&&) noexcept;

    // Constructor
public:
    ApiWs();

    // Methods
public:
    void Demobilize();

    /**
     * This method forms a new subscription to diagnostic
     * messages published by the class.
     *
     * @param[in] delegate
     *     This is the function to call to deliver messages
     *     to the subscriber.
     *
     * @param[in] minLevel
     *     This is the minimum level of message that this subscriber
     *     desires to receive.
     *
     * @return
     *     A function is returned which may be called
     *     to terminate the subscription.
     */
    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
        size_t minLevel = 0
    );

    void Mobilize(
        const std::shared_ptr< Store >& store,
        const std::shared_ptr< Http::Server >& httpServer,
        const std::shared_ptr< Timekeeping::Clock >& clock,
        const Json::Value& configuration
    );

    // Private properties
private:
    /**
     * This is the type of structure that contains the private
     * properties of the instance.  It is defined in the implementation
     * and declared here to ensure that it is scoped inside the class.
     */
    struct Impl;

    /**
     * This contains the private properties of the instance.
     */
    std::shared_ptr< Impl > impl_;
};
