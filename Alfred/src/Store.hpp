#pragma once

/**
 * @file Store.hpp
 *
 * This module declares the Store implementation.
 */

#include <Json/Value.hpp>
#include <memory>
#include <string>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <Timekeeping/Clock.hpp>
#include <unordered_set>
#include <vector>

/**
 * This object manages all the data (and its metadata) that Alfred holds
 * and distributes to clients.
 */
class Store {
    // Lifecycle Methods
public:
    ~Store() noexcept;
    Store(const Store&) = delete;
    Store(Store&&) noexcept;
    Store& operator=(const Store&) = delete;
    Store& operator=(Store&&) noexcept;

    // Constructor
public:
    Store();

    // Methods
public:
    void Demobilize();

    Json::Value GetData(
        const std::vector< std::string >& path,
        const std::unordered_set< std::string >& rolesHeld
    );

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

    bool Mobilize(
        const std::string& filePath,
        std::shared_ptr< Timekeeping::Clock > clock
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
