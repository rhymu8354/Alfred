/**
 * @file ApiHttp.cpp
 *
 * This module contains the implementation of functions which implement an
 * Application Programming Interface (API) to the service via Hypertext
 * Transfer Protocol (HTTP).
 */

#include "ApiHttp.hpp"

#include <functional>
#include <Json/Value.hpp>
#include <memory>

#include <unordered_set>

namespace {

    using Handler = std::function<
        Json::Value(
            const std::shared_ptr< Store >& store,
            const Http::Request& request,
            Http::Response& response
        )
    >;

    struct HandlerRegistration {
        HandlerRegistration* next;
        std::vector< std::string > resourceSubspacePath;
        std::unordered_set< std::string > methods;
        Handler handler;
    };

    HandlerRegistration* handlerRegistrations = nullptr;

    struct AddHandlerRegistration {
        AddHandlerRegistration(
            HandlerRegistration* handlerRegistration,
            Handler&& handler,
            std::unordered_set< std::string >&& methods,
            std::vector< std::string >&& resourceSubspacePath
        ) {
            handlerRegistration->next = handlerRegistrations;
            handlerRegistration->resourceSubspacePath = std::move(resourceSubspacePath);
            handlerRegistration->methods = std::move(methods);
            handlerRegistration->handler = std::move(handler);
            handlerRegistrations = handlerRegistration;
        }
    };

    #define DEFINE_HANDLER(handler) \
        Json::Value handler( \
            const std::shared_ptr< Store >& store, \
            const Http::Request& request, \
            Http::Response& response \
        ); \
        HandlerRegistration HandlerRegistration##handler; \
        const AddHandlerRegistration AddHandlerRegistration##handler(\
            &HandlerRegistration##handler, \
            handler, \
            HANDLER_METHODS, \
            HANDLER_PATH \
        ); \
        Json::Value handler( \
            const std::shared_ptr< Store >& store, \
            const Http::Request& request, \
            Http::Response& response \
        )

    #undef HANDLER_METHODS
    #undef HANDLER_PATH
    #define HANDLER_METHODS {"GET", "PUT", "POST", "DELETE"}
    #define HANDLER_PATH {}
    DEFINE_HANDLER(Unknown){
        response.statusCode = 404;
        response.reasonPhrase = "Not Found";
        return Json::Object({
            {"message", "No such resource defined"},
        });
    }

    #undef HANDLER_METHODS
    #undef HANDLER_PATH
    #define HANDLER_METHODS {"GET"}
    #define HANDLER_PATH {"data"}
    DEFINE_HANDLER(Test){
        return store->GetData(
            request.target.GetPath(),
            {"public"}
        );
    }

    #undef DEFINE_HANDLER
    #undef HANDLER_METHODS
    #undef HANDLER_PATH
}

namespace ApiHttp {

    void RegisterResources(
        const std::shared_ptr< Store >& store,
        Http::Server& httpServer
    ) {
        std::weak_ptr< Store > storeWeak(store);
        for (
            auto handlerRegistration = handlerRegistrations;
            handlerRegistration != nullptr;
            handlerRegistration = handlerRegistration->next
        ) {
            const auto handler = handlerRegistration->handler;
            const auto methods = handlerRegistration->methods;
            (void)httpServer.RegisterResource(
                handlerRegistration->resourceSubspacePath,
                [storeWeak, handler, methods](
                    const Http::Request& request,
                    std::shared_ptr< Http::Connection > connection,
                    const std::string& trailer
                ){
                    Http::Response response;
                    const auto store = storeWeak.lock();
                    if (store == nullptr) {
                        response.statusCode = 503;
                        response.reasonPhrase = "Service Unavailable";
                        response.body = Json::Object({
                            {"message", "The service is shutting down.  Please try again later!"},
                        }).ToEncoding();
                    } else if (methods.find(request.method) == methods.end()) {
                        response.statusCode = 405;
                        response.reasonPhrase = "Method Not Allowed";
                    } else {
                        response.statusCode = 200;
                        response.reasonPhrase = "OK";
                        response.body = handler(store, request, response).ToEncoding();
                    }
                    if (response.body.empty()) {
                        response.headers.SetHeader("Content-Length", "0");
                    } else {
                        response.headers.SetHeader("Content-Type", "application/json");
                    }
                    if (response.statusCode / 100 == 2) {
                        response.headers.SetHeader("Access-Control-Allow-Origin", "*");
                    }
                    return response;
                }
            );
        }
    }

}
