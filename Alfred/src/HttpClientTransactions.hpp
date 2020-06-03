#pragma once

/**
 * @file HttpClientTransactions.hpp
 *
 * This module declares the HttpClientTransactions class which uses an
 * Http::Client object to create and complete request-response transactions.
 */

#include <Http/Client.hpp>
#include <memory>
#include <SystemAbstractions/DiagnosticsSender.hpp>

class HttpClientTransactions {
    // Types
public:
    using CompletionDelegate = std::function< void(Http::Response& response) >;

    // Lifecycle
public:
    ~HttpClientTransactions() noexcept;
    HttpClientTransactions(const HttpClientTransactions&) = delete;
    HttpClientTransactions(HttpClientTransactions&&) noexcept;
    HttpClientTransactions& operator=(const HttpClientTransactions&) = delete;
    HttpClientTransactions& operator=(HttpClientTransactions&&) noexcept;

    // Constructor
public:
    HttpClientTransactions();

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
        const std::shared_ptr< Http::Client >& httpClient
    );

    void Post(
        Http::Request& request,
        CompletionDelegate completionDelegate
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
