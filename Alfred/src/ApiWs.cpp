/**
 * @file ApiWs.cpp
 *
 * This module contains the implementation of the ApiWs class which manages an
 * Application Programming Interface (API) to the service via WebSockets (WS).
 */

#include "ApiWs.hpp"

#include <functional>
#include <Http/Server.hpp>
#include <Json/Value.hpp>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <StringExtensions/StringExtensions.hpp>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <Timekeeping/Scheduler.hpp>
#include <unordered_map>
#include <WebSockets/WebSocket.hpp>

namespace {

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

        // Properties

        bool authenticated = false;
        int authenticationTimeout = 0;
        CloseDelegate closeDelegate;
        SystemAbstractions::DiagnosticsSender diagnosticsSender;
        std::mutex mutex;
        std::shared_ptr< Timekeeping::Scheduler > scheduler;
        std::shared_ptr< Store > store;
        std::weak_ptr< WebSockets::WebSocket > wsWeak;

        // Constructor

        Client(
            const std::string& peerId,
            const std::weak_ptr< WebSockets::WebSocket >& wsWeak,
            const std::shared_ptr< Store >& store,
            const std::shared_ptr< Timekeeping::Scheduler >& scheduler,
            CloseDelegate closeDelegate
        )
            : closeDelegate(closeDelegate)
            , diagnosticsSender(peerId)
            , scheduler(scheduler)
            , store(store)
            , wsWeak(wsWeak)
        {
        }

        // Methods

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
            const auto ws = wsWeak.lock();
            if (ws != nullptr) {
                ws->SendText(Json::Object({
                    {"type", "Error"},
                    {"message", "Authentication timeout"},
                }).ToEncoding());
            }
            auto closeDelegateCopy = closeDelegate;
            lock.unlock();
            if (closeDelegateCopy) {
                closeDelegateCopy(1005, "");
            }
        }

        void OnGreeting(
            const Json::Value& message,
            std::unique_lock< decltype(mutex) >& lock
        ) {
            authenticated = true;
            if (authenticationTimeout) {
                scheduler->Cancel(authenticationTimeout);
                authenticationTimeout = 0;
            }
            const auto ws = wsWeak.lock();
            if (ws != nullptr) {
                ws->SendText(Json::Object({
                    {"type", "Notice"},
                    {"message", StringExtensions::sprintf(
                        "You said this: %s",
                        message.ToEncoding().c_str()
                    )},
                }).ToEncoding());
            }
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
                if (ws != nullptr) {
                    ws->SendText(Json::Object({
                        {"type", "Error"},
                        {"message", "malformed message received"}
                    }).ToEncoding());
                }
                auto closeDelegateCopy = closeDelegate;
                lock.unlock();
                if (closeDelegateCopy) {
                    closeDelegateCopy(1005, "");
                }
                return;
            }
            const auto messageType = (std::string)message["type"];
            static const std::unordered_map< std::string, MessageHandler > messageHandlers{
                {"Greeting", &Client::OnGreeting},
            };
            const auto messageHandler = messageHandlers.find(messageType);
            if (messageHandler == messageHandlers.end()) {
                if (ws != nullptr) {
                    ws->SendText(Json::Object({
                        {"type", "Error"},
                        {
                            "message",
                            StringExtensions::sprintf(
                                "Unknown message type received: %s",
                                messageType.c_str()
                            )
                        },
                    }).ToEncoding());
                }
            } else {
                (this->*messageHandler->second)(message, lock);
            }
        }

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
    const std::shared_ptr< Http::Server >& httpServer,
    const std::shared_ptr< Timekeeping::Clock >& clock,
    const Json::Value& configuration
) {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    if (impl_->mobilized) {
        return;
    }
    impl_->store = store;
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
