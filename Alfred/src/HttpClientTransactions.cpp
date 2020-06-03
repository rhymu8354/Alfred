/**
 * @file HttpClientTransactions.cpp
 *
 * This module contains the implementation of the HttpClientTransactions class
 * which uses an Http::Client object to create and complete request-response
 * transactions.
 */

#include "HttpClientTransactions.hpp"

#include <memory>
#include <mutex>
#include <unordered_set>

/**
 * This contains the private properties of a HttpClientTransactions class instance.
 */
struct HttpClientTransactions::Impl
    : public std::enable_shared_from_this< HttpClientTransactions::Impl >
{
    // Properties

    SystemAbstractions::DiagnosticsSender diagnosticsSender;
    std::shared_ptr< Http::Client > httpClient;
    std::mutex mutex;
    int nextTransactionId = 1;
    std::unordered_set< std::shared_ptr< Http::IClient::Transaction > > transactions;

    // Constructor

    Impl()
        : diagnosticsSender("HttpClientTransactions")
    {
    }

    void OnCompletion(
        int id,
        const std::shared_ptr< Http::IClient::Transaction >& transaction,
        CompletionDelegate completionDelegate
    ) {
        std::unique_lock< decltype(mutex) > lock(mutex);
        diagnosticsSender.SendDiagnosticInformationFormatted(
            0,
            "%d reply: %u (%s)",
            id,
            transaction->response.statusCode,
            transaction->response.reasonPhrase.c_str()
        );
        auto response = std::move(transaction->response);
        (void)transactions.erase(transaction);
        lock.unlock();
        completionDelegate(response);
    }

};

HttpClientTransactions::~HttpClientTransactions() noexcept {
    Demobilize();
}

HttpClientTransactions::HttpClientTransactions(HttpClientTransactions&&) noexcept = default;
HttpClientTransactions& HttpClientTransactions::operator=(HttpClientTransactions&&) noexcept = default;

HttpClientTransactions::HttpClientTransactions()
    : impl_(new Impl())
{
}

void HttpClientTransactions::Demobilize() {
    impl_->httpClient = nullptr;
    impl_->transactions.clear();
}

SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate HttpClientTransactions::SubscribeToDiagnostics(
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
    size_t minLevel
) {
    return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
}

void HttpClientTransactions::Mobilize(
    const std::shared_ptr< Http::Client >& httpClient
) {
    impl_->httpClient = httpClient;
}

void HttpClientTransactions::Post(
    Http::Request& request,
    CompletionDelegate completionDelegate
) {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    if (!request.target.HasPort()) {
        const auto scheme = request.target.GetScheme();
        if (
            (scheme == "https")
            || (scheme == "wss")
        ) {
            request.target.SetPort(443);
        }
    }
    const auto id = impl_->nextTransactionId++;
    impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
        0,
        "%d request: %s",
        id,
        request.target.GenerateString().c_str()
    );
    auto transaction = impl_->httpClient->Request(request);
    (void)impl_->transactions.insert(transaction);
    std::weak_ptr< Http::IClient::Transaction > transactionWeak(transaction);
    std::weak_ptr< Impl > implWeak(impl_);
    transaction->SetCompletionDelegate(
        [
            completionDelegate,
            id,
            implWeak,
            transactionWeak
        ]{
            auto impl = implWeak.lock();
            if (impl == nullptr) {
                return;
            }
            auto transaction = transactionWeak.lock();
            if (transaction == nullptr) {
                impl->diagnosticsSender.SendDiagnosticInformationFormatted(
                    SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                    "%d abandoned",
                    id
                );
                return;
            }
            impl->OnCompletion(id, transaction, completionDelegate);
        }
    );
}
