/**
 * @file Store.cpp
 *
 * This module contains the implementation of the Store class.
 */

#include "LoadFile.hpp"
#include "Store.hpp"

#include <Json/Value.hpp>
#include <mutex>
#include <Timekeeping/Scheduler.hpp>
#include <StringExtensions/StringExtensions.hpp>
#include <SystemAbstractions/DiagnosticsSender.hpp>

namespace {

    constexpr double defaultMinSaveInterval = 60.0;

    /**
     * Return a reference to the JSON object containing the key at
     * the given "path".  The path consists of JSON object keys separated
     * by dots.
     *
     * @param[in] root
     *     This is the top-level JSON object in which to find a
     *     descendant node.
     *
     * @param[in] path
     *     This is the sequence of keys to use to descend the
     *     tree.
     *
     * @param[in] offset
     *     This is the number of characters to skip in `path`.
     *
     * @return
     *     A reference to the "descendant" JSON object containing the
     *     last key in the given sequence is returned.
     */
    const Json::Value& DescendTree(
        const Json::Value& root,
        const std::string& path,
        size_t offset = 0
    ) {
        if (offset >= path.length()) {
            return root;
        }
        auto delimiter = path.find('.', offset);
        if (delimiter == std::string::npos) {
            delimiter = path.length();
        }
        const auto key = path.substr(offset, delimiter - offset);
        if (root.Has("data")) {
            return DescendTree(root["data"][key], path, delimiter + 1);
        } else {
            return DescendTree(root[key], path, delimiter + 1);
        }
    }

    Json::Value ExtractData(const Json::Value& root) {
        if (
            (root.GetType() == Json::Value::Type::Object)
            && (root.Has("data"))
        ) {
            return ExtractData(root["data"]);
        }
        switch (root.GetType()) {
            case Json::Value::Type::Array: {
                auto data = Json::Array({});
                for (const auto entry: root) {
                    data.Add(ExtractData(entry.value()));
                }
                return data;
            }

            case Json::Value::Type::Object: {
                auto data = Json::Object({});
                for (const auto entry: root) {
                    data[entry.key()] = ExtractData(entry.value());
                }
                return data;
            }

            default: return root;
        }
    }

}

struct Store::Impl
    : public std::enable_shared_from_this< Store::Impl >
{
    // Properties

    SystemAbstractions::DiagnosticsSender diagnosticsSender;
    bool dirty = false;
    double minSaveInterval = 0.0;
    size_t generation = 0;
    bool mobilized = false;
    std::mutex mutex;
    double nextSaveTime = 0.0;
    int nextSaveToken = 0;
    bool saving = false;
    Json::Value store;
    Timekeeping::Scheduler scheduler;

    // Constructor

    Impl()
        : diagnosticsSender("Store")
    {
    }

    // Methods

    Json::Value GetData(const std::string& path) {
        const auto& root = DescendTree(store, path);
        return ExtractData(root);
    }

    void Save() {
        saving = false;
        if (dirty) {
            ScheduleSave();
        }
    }

    void ScheduleSave() {
        if (saving) {
            dirty = true;
            return;
        }
        saving = true;
        dirty = false;
        const auto now = scheduler.GetClock()->GetCurrentTime();
        if (nextSaveTime < now) {
            nextSaveTime = now;
        }
        std::weak_ptr< Impl > selfWeak(shared_from_this());
        const auto thisGeneration = generation;
        nextSaveToken = scheduler.Schedule(
            [selfWeak, thisGeneration]{
                const auto self = selfWeak.lock();
                if (self == nullptr) {
                    return;
                }
                std::lock_guard< decltype(self->mutex) > lock(self->mutex);
                if (
                    !self->mobilized
                    || (self->generation != thisGeneration)
                ) {
                    return;
                }
                self->Save();
            },
            nextSaveTime
        );
        nextSaveTime += minSaveInterval;
    }
};

Store::~Store() noexcept {
    Demobilize();
}

Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

Store::Store()
    : impl_(new Impl())
{
}

void Store::Demobilize() {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    if (!impl_->mobilized) {
        return;
    }
    if (impl_->saving) {
        impl_->scheduler.Cancel(impl_->nextSaveToken);
        impl_->saving = false;
    }
    impl_->dirty = false;
    impl_->scheduler.SetClock(nullptr);
    impl_->mobilized = false;
}

Json::Value Store::GetData(const std::string& path) {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    return impl_->GetData(path);
}

SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate Store::SubscribeToDiagnostics(
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
    size_t minLevel
) {
    return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
}

bool Store::Mobilize(
    const std::string& filePath,
    std::shared_ptr< Timekeeping::Clock > clock
) {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    if (impl_->mobilized) {
        return true;
    }
    std::string encodedStore;
    if (
        !LoadFile(
            filePath,
            "store",
            impl_->diagnosticsSender,
            encodedStore
        )
    ) {
        return false;
    }
    impl_->store = Json::Value::FromEncoding(encodedStore);
    if (impl_->store.GetType() == Json::Value::Type::Invalid) {
        impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
            "Unable to parse from file '%s'",
            filePath.c_str()
        );
        return false;
    }
    impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
        3,
        "Loaded from file '%s'",
        filePath.c_str()
    );
    const auto configuration = impl_->GetData("Configuration");
    if (configuration.Has("MinSaveInterval")){
        impl_->minSaveInterval = configuration["MinSaveInterval"];
    } else {
        impl_->minSaveInterval = defaultMinSaveInterval;
    }
    impl_->scheduler.SetClock(clock);
    impl_->mobilized = true;
    ++impl_->generation;
    return true;
}
