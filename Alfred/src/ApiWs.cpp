/**
 * @file ApiWs.cpp
 *
 * This module contains the implementation of the ApiWs class which manages an
 * Application Programming Interface (API) to the service via WebSockets (WS).
 */

#include "ApiWs.hpp"

#include <algorithm>
#include <functional>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <Json/Value.hpp>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <string>
#include <StringExtensions/StringExtensions.hpp>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <Timekeeping/Scheduler.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <WebSockets/WebSocket.hpp>

namespace {

    std::vector< std::string > Sorted(const std::unordered_set< std::string >& unsorted) {
        std::vector< std::string > sorted(
            unsorted.begin(),
            unsorted.end()
        );
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    }

    struct Client
        : public std::enable_shared_from_this< Client >
    {
        // Types

        using CloseDelegate = std::function<
            void(
                unsigned int code,
                const std::string& reason
            )
        >;

        using MessageHandler = void (Client::*)(
            const Json::Value& data,
            std::unique_lock< std::mutex >& lock
        );

        #define DEFINE_MESSAGE_HANDLER(x) void x( \
            const Json::Value& message, \
            std::unique_lock< decltype(mutex) >& lock \
        )

        // Properties

        bool authenticated = false;
        int authenticationTimeout = 0;
        CloseDelegate closeDelegate;
        SystemAbstractions::DiagnosticsSender diagnosticsSender;
        std::shared_ptr< Http::Client > httpClient;
        std::unordered_map< int, std::shared_ptr< Http::IClient::Transaction > > httpClientTransactions;
        std::unordered_set< std::string > identifiers;
        static const std::unordered_map< std::string, MessageHandler > messageHandlers;
        std::mutex mutex;
        int nextHttpClientTransactionId = 1;
        std::unordered_set< std::string > roles;
        std::shared_ptr< Timekeeping::Scheduler > scheduler;
        std::shared_ptr< Store > store;
        std::weak_ptr< WebSockets::WebSocket > wsWeak;

        // Constructor

        Client(
            const std::string& peerId,
            const std::weak_ptr< WebSockets::WebSocket >& wsWeak,
            const std::shared_ptr< Http::Client >& httpClient,
            const std::shared_ptr< Store >& store,
            const std::shared_ptr< Timekeeping::Scheduler >& scheduler,
            CloseDelegate closeDelegate
        )
            : closeDelegate(closeDelegate)
            , diagnosticsSender(peerId)
            , scheduler(scheduler)
            , httpClient(httpClient)
            , store(store)
            , wsWeak(wsWeak)
        {
        }

        // Methods

        void AddRole(const std::string& role) {
            if (roles.insert(role).second) {
                diagnosticsSender.SendDiagnosticInformationString(
                    2,
                    std::string("Role added: ") + role
                );
            }
        }

        void AddIdentifier(const std::string& identifier) {
            if (identifiers.insert(identifier).second) {
                diagnosticsSender.SendDiagnosticInformationString(
                    2,
                    std::string("Identifier added: ") + identifier
                );
                const auto roles = store->GetData("Roles");
                if (roles.Has(identifier)) {
                    for (const auto rolesEntry: roles[identifier]) {
                        const auto role = (std::string)rolesEntry.value();
                        AddRole(role);
                    }
                }
            }
        }

        DEFINE_MESSAGE_HANDLER(OnAuthenticate) {
            if (authenticated) {
                ReportError("Already authenticated; reconnect to reauthenticate", lock);
                return;
            }
            if (message.Has("key")) {
                const auto identifier = std::string("key:") + (std::string)message["key"];
                const auto roles = store->GetData("Roles");
                if (roles.Has(identifier)) {
                    AddIdentifier(identifier);
                } else {
                    ReportError("Invalid access key", lock, true);
                    return;
                }
            } else if (message.Has("twitch")) {
                ValidateOAuthToken(
                    message["twitch"],
                    [](Client& self, std::unique_lock< std::mutex >& lock, intmax_t twitchId){
                        const auto identifier = StringExtensions::sprintf(
                            "twitch:%" PRIdMAX,
                            twitchId
                        );
                        const auto roles = self.store->GetData("Roles");
                        self.AddIdentifier(identifier);
                        self.OnAuthenticated();
                    },
                    [](Client& self, std::unique_lock< std::mutex >& lock){
                        self.ReportError("Invalid OAuth token", lock, true);
                    }
                );
            } else {
                ReportError("Unrecognized authentication method", lock, true);
                return;
            }
            if (!identifiers.empty()) {
                OnAuthenticated();
            }
        }

        void OnAuthenticated() {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                3,
                "Authenticated, identifiers: %s; roles: %s",
                StringExtensions::Join(Sorted(identifiers), ", ").c_str(),
                StringExtensions::Join(Sorted(roles), ", ").c_str()
            );
            authenticated = true;
            if (authenticationTimeout) {
                scheduler->Cancel(authenticationTimeout);
                authenticationTimeout = 0;
            }
            const auto ws = wsWeak.lock();
            if (ws != nullptr) {
                ws->SendText(Json::Object({
                    {"type", "Authenticated"},
                }).ToEncoding());
            }
        }

        void OnAuthenticationTimeout() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            authenticationTimeout = 0;
            if (authenticated) {
                return;
            }
            diagnosticsSender.SendDiagnosticInformationString(
                SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                "Authentication timeout"
            );
            ReportError("Authentication timeout", lock, true);
        }

        void OnOpened() {
            std::lock_guard< decltype(mutex) > lock(mutex);
            diagnosticsSender.SendDiagnosticInformationString(
                2,
                "Opened"
            );
            std::weak_ptr< Client > selfWeak(shared_from_this());
            const auto configuration = store->GetData("Configuration");
            authenticationTimeout = scheduler->Schedule(
                [
                    selfWeak
                ]{
                    const auto self = selfWeak.lock();
                    if (self == nullptr) {
                        return;
                    }
                    self->OnAuthenticationTimeout();
                },
                scheduler->GetClock()->GetCurrentTime() + (double)configuration["WebSocketAuthenticationTimeout"]
            );
        }

        void OnClosed(
            unsigned int code,
            const std::string& reason
        ) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            diagnosticsSender.SendDiagnosticInformationFormatted(
                2,
                "Closed (code %u, reason: \"%s\")",
                code,
                reason.c_str()
            );
        }

        void OnText(const std::string& data) {
            std::unique_lock< decltype(mutex) > lock(mutex);
            diagnosticsSender.SendDiagnosticInformationFormatted(
                0,
                "Received: \"%s\"",
                data.c_str()
            );
            const auto ws = wsWeak.lock();
            const auto message = Json::Value::FromEncoding(data);
            if (
                (message.GetType() != Json::Value::Type::Object)
                || !message.Has("type")
            ) {
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                    "Malformed message received: \"%s\"",
                    data.c_str()
                );
                ReportError("malformed message received", lock, true);
                return;
            }
            const auto messageType = (std::string)message["type"];
            const auto messageHandler = messageHandlers.find(messageType);
            if (messageHandler == messageHandlers.end()) {
                ReportError(
                    StringExtensions::sprintf(
                        "Unknown message type received: %s",
                        messageType.c_str()
                    ),
                    lock
                );
            } else {
                (this->*messageHandler->second)(message, lock);
            }
        }

        void PostHttpClientTransaction(
            Http::Request& request,
            std::function< void(Client& self, std::unique_lock< std::mutex >& lock, Http::Response& response) > onCompletion
        ) {
            if (!request.target.HasPort()) {
                const auto scheme = request.target.GetScheme();
                if (
                    (scheme == "https")
                    || (scheme == "wss")
                ) {
                    request.target.SetPort(443);
                }
            }
            const auto id = nextHttpClientTransactionId++;
            diagnosticsSender.SendDiagnosticInformationFormatted(
                0,
                "HTTP Request %d: %s",
                id,
                request.target.GenerateString().c_str()
            );
            auto& httpClientTransaction = httpClientTransactions[id];
            httpClientTransaction = httpClient->Request(request);
            std::weak_ptr< Client > selfWeak(shared_from_this());
            httpClientTransaction->SetCompletionDelegate(
                [
                    id,
                    onCompletion,
                    selfWeak
                ]{
                    auto self = selfWeak.lock();
                    if (self == nullptr) {
                        return;
                    }
                    std::unique_lock< decltype(self->mutex) > lock(self->mutex);
                    auto httpClientTransactionsEntry = self->httpClientTransactions.find(id);
                    if (httpClientTransactionsEntry == self->httpClientTransactions.end()) {
                        return;
                    }
                    const auto& httpClientTransaction = httpClientTransactionsEntry->second;
                    self->diagnosticsSender.SendDiagnosticInformationFormatted(
                        0,
                        "HTTP Reply %d: %u (%s)",
                        id,
                        httpClientTransaction->response.statusCode,
                        httpClientTransaction->response.reasonPhrase.c_str()
                    );
                    auto response = std::move(httpClientTransaction->response);
                    (void)self->httpClientTransactions.erase(httpClientTransactionsEntry);
                    onCompletion(*self, lock, response);
                }
            );
        }

        void ReportError(
            const std::string& message,
            std::unique_lock< std::mutex >& lock,
            bool disconnect = false
        ) {
            const auto ws = wsWeak.lock();
            if (ws != nullptr) {
                ws->SendText(Json::Object({
                    {"type", "Error"},
                    {"message", message},
                }).ToEncoding());
            }
            if (disconnect) {
                auto closeDelegateCopy = closeDelegate;
                lock.unlock();
                if (closeDelegateCopy) {
                    closeDelegateCopy(1005, "");
                }
            }
        }

        void ValidateOAuthToken(
            const std::string& token,
            std::function< void(Client& self, std::unique_lock< std::mutex >& lock, intmax_t twitchId) > onSuccess,
            std::function< void(Client& self, std::unique_lock< std::mutex >& lock) > onFailure
        ) {
            Http::Request request;
            request.method = "GET";
            request.target.ParseFromString("https://id.twitch.tv/oauth2/validate");
            request.headers.SetHeader(
                "Authorization",
                std::string("OAuth ") + token
            );
            std::weak_ptr< Client > selfWeak(shared_from_this());
            PostHttpClientTransaction(
                request,
                [
                    onFailure,
                    onSuccess,
                    selfWeak
                ](Client& self, std::unique_lock< std::mutex >& lock, const Http::Response& response){
                    if (response.statusCode == 200) {
                        const auto data = Json::Value::FromEncoding(response.body);
                        intmax_t twitchId;
                        if (
                            sscanf(
                                ((std::string)data["user_id"]).c_str(),
                                "%" SCNdMAX,
                                &twitchId
                            ) == 1
                        ) {
                            onSuccess(self, lock, twitchId);
                            return;
                        }
                    }
                    onFailure(self, lock);
                }
            );
        }

    };

    const std::unordered_map< std::string, Client::MessageHandler > Client::messageHandlers{
        {"Authenticate", &Client::OnAuthenticate},
    };

}

/**
 * This contains the private properties of a ApiWs class instance.
 */
struct ApiWs::Impl
    : public std::enable_shared_from_this< ApiWs::Impl >
{
    // Properties

    std::unordered_map<
        std::shared_ptr< WebSockets::WebSocket >,
        std::shared_ptr< Client >
    > clients;
    SystemAbstractions::DiagnosticsSender diagnosticsSender;
    size_t generation = 0;
    std::shared_ptr< Http::Client > httpClient;
    std::shared_ptr< Http::Server > httpServer;
    bool mobilized = false;
    std::recursive_mutex mutex;
    Http::IServer::UnregistrationDelegate resourceUnregistrationDelegate;
    std::shared_ptr< Timekeeping::Scheduler > scheduler;
    std::shared_ptr< Store > store;

    // Constructor

    Impl()
        : diagnosticsSender("ApiWs")
    {
    }

    // Methods

    void CloseWebSocket(
        const std::shared_ptr< WebSockets::WebSocket >& ws,
        unsigned int code = 1005,
        const std::string& reason = ""
    ) {
        const auto clientsEntry = clients.find(ws);
        if (
            (clientsEntry == clients.end())
            || (clientsEntry->second == nullptr)
        ) {
            return;
        }
        ws->Close(code, reason);
        clientsEntry->second->OnClosed(code, reason);
        clientsEntry->second = nullptr;
        const auto thisGeneration = generation;
        const auto configuration = store->GetData("Configuration");
        const auto webSocketCloseLinger = (double)configuration["WebSocketCloseLinger"];
        std::weak_ptr< WebSockets::WebSocket > wsWeak(ws);
        std::weak_ptr< Impl > implWeak(shared_from_this());
        (void)scheduler->Schedule(
            [
                implWeak,
                thisGeneration,
                wsWeak
            ]{
                const auto ws = wsWeak.lock();
                if (ws == nullptr) {
                    return;
                }
                const auto impl = implWeak.lock();
                if (impl == nullptr) {
                    return;
                }
                std::lock_guard< decltype(impl->mutex) > lock(impl->mutex);
                if (
                    !impl->mobilized
                    || (impl->generation != thisGeneration)
                ) {
                    return;
                }
                const auto clientsEntry = impl->clients.find(ws);
                if (clientsEntry == impl->clients.end()) {
                    return;
                }
                impl->diagnosticsSender.SendDiagnosticInformationString(
                    0,
                    "Dropping WebSocket"
                );
                impl->clients.erase(clientsEntry);
            },
            scheduler->GetClock()->GetCurrentTime() + webSocketCloseLinger
        );
    }

    Http::Response HandleWebSocketRequest(
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer,
        const Json::Value& configuration
    ) {
        Http::Response response;
        const auto ws = std::make_shared< WebSockets::WebSocket >();
        WebSockets::WebSocket::Configuration webSocketConfiguration;
        webSocketConfiguration.maxFrameSize = configuration["WebSocketMaxFrameSize"];
        ws->Configure(webSocketConfiguration);
        (void)ws->SubscribeToDiagnostics(diagnosticsSender.Chain());
        response.statusCode = 0;
        if (ws->OpenAsServer(connection, request, response, trailer)) {
            auto& client = clients[ws];
            std::weak_ptr< Impl > implWeak(shared_from_this());
            std::weak_ptr< WebSockets::WebSocket > wsWeak(ws);
            client = std::make_shared< Client >(
                connection->GetPeerId(),
                wsWeak,
                httpClient,
                store,
                scheduler,
                [implWeak, wsWeak](
                    unsigned int code,
                    const std::string& reason
                ){
                    const auto impl = implWeak.lock();
                    if (impl == nullptr) {
                        return;
                    }
                    const auto ws = wsWeak.lock();
                    if (ws == nullptr) {
                        return;
                    }
                    std::lock_guard< decltype(impl->mutex) > lock(impl->mutex);
                    impl->CloseWebSocket(ws, code, reason);
                }
            );
            (void)client->diagnosticsSender.SubscribeToDiagnostics(
                diagnosticsSender.Chain(),
                configuration["DiagnosticReportingThresholds"]["WebSocket"]
            );
            client->OnOpened();
            WebSockets::WebSocket::Delegates delegates;
            const auto thisGeneration = generation;
            delegates.close = [
                implWeak,
                thisGeneration,
                wsWeak
            ](
                unsigned int code,
                const std::string& reason
            ){
                const auto impl = implWeak.lock();
                if (impl == nullptr) {
                    return;
                }
                const auto ws = wsWeak.lock();
                if (ws == nullptr) {
                    return;
                }
                std::lock_guard< decltype(impl->mutex) > lock(impl->mutex);
                if (
                    !impl->mobilized
                    || (impl->generation != thisGeneration)
                ) {
                    return;
                }
                impl->OnWebSocketClosed(ws, code, reason);
            };
            delegates.text = [
                implWeak,
                thisGeneration,
                wsWeak
            ](
                const std::string& data
            ){
                const auto impl = implWeak.lock();
                if (impl == nullptr) {
                    return;
                }
                const auto ws = wsWeak.lock();
                if (ws == nullptr) {
                    return;
                }
                std::lock_guard< decltype(impl->mutex) > lock(impl->mutex);
                if (
                    !impl->mobilized
                    || (impl->generation != thisGeneration)
                ) {
                    return;
                }
                impl->OnWebSocketText(ws, data);
            };
            ws->SetDelegates(std::move(delegates));
        } else if (response.statusCode == 0) {
            response.statusCode = 426;
            response.reasonPhrase = "Upgrade Required";
            response.headers.SetHeader("Upgrade", "websocket");
            response.headers.SetHeader("Content-Length", "0");
        }
        return response;
    }

    void OnWebSocketClosed(
        std::shared_ptr< WebSockets::WebSocket > ws,
        unsigned int code,
        const std::string& reason
    ) {
        CloseWebSocket(ws);
    }

    void OnWebSocketText(
        std::shared_ptr< WebSockets::WebSocket > ws,
        const std::string& data
    ) {
        auto clientsEntry = clients.find(ws);
        if (clientsEntry == clients.end()) {
            return;
        }
        clientsEntry->second->OnText(data);
    }

};

ApiWs::~ApiWs() noexcept {
    Demobilize();
}

ApiWs::ApiWs(ApiWs&&) noexcept = default;
ApiWs& ApiWs::operator=(ApiWs&&) noexcept = default;

ApiWs::ApiWs()
    : impl_(new Impl())
{
}

void ApiWs::Demobilize() {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    if (!impl_->mobilized) {
        return;
    }
    for (auto& client: impl_->clients) {
        impl_->CloseWebSocket(client.first);
    }
    impl_->clients.clear();
    impl_->resourceUnregistrationDelegate();
    impl_->httpServer = nullptr;
    impl_->scheduler = nullptr;
    impl_->store = nullptr;
    impl_->mobilized = false;
}

SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate ApiWs::SubscribeToDiagnostics(
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
    size_t minLevel
) {
    return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
}

void ApiWs::Mobilize(
    const std::shared_ptr< Store >& store,
    const std::shared_ptr< Http::Client >& httpClient,
    const std::shared_ptr< Http::Server >& httpServer,
    const std::shared_ptr< Timekeeping::Clock >& clock,
    const Json::Value& configuration
) {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    if (impl_->mobilized) {
        return;
    }
    impl_->store = store;
    impl_->httpClient = httpClient;
    impl_->httpServer = httpServer;
    impl_->scheduler = std::make_shared< Timekeeping::Scheduler >();
    impl_->scheduler->SetClock(clock);
    std::weak_ptr< Impl > implWeak(impl_);
    impl_->resourceUnregistrationDelegate = impl_->httpServer->RegisterResource(
        {"ws"},
        [
            configuration,
            implWeak
        ](
            const Http::Request& request,
            std::shared_ptr< Http::Connection > connection,
            const std::string& trailer
        ){
            const auto impl = implWeak.lock();
            if (impl == nullptr) {
                Http::Response response;
                response.statusCode = 503;
                response.reasonPhrase = "Service Unavailable";
                response.body = Json::Object({
                    {"message", "The service is shutting down.  Please try again later!"},
                }).ToEncoding();
                return response;
            }
            std::lock_guard< decltype(impl->mutex) > lock(impl->mutex);
            return impl->HandleWebSocketRequest(request, connection, trailer, configuration);
        }
    );
    ++impl_->generation;
    impl_->mobilized = true;
}
