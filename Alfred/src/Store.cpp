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
#include <unordered_map>
#include <unordered_set>

namespace {

    constexpr double defaultMinSaveInterval = 60.0;

    struct RolesPermitted {
        std::unordered_set< std::string > readData;
        std::unordered_set< std::string > readMeta;
        std::unordered_set< std::string > writeData;
        std::unordered_set< std::string > writeMeta;
        std::unordered_set< std::string > createData;
        std::unordered_set< std::string > deleteData;
    };

    // Forward declaractions
    Json::Value ExtractData(
        const RolesPermitted& rolesPermitted,
        const Json::Value& root,
        const std::unordered_set< std::string >& rolesHeld
    );
    bool RolePermitted(
        const std::unordered_set< std::string >& lhs,
        const std::unordered_set< std::string >& rhs
    );
    void UpdateRoles(
        RolesPermitted& rolesPermitted,
        const Json::Value& meta
    );

    void AddRoles(
        std::unordered_set< std::string >& rolesSet,
        const Json::Value& rolesArray
    ) {
        if (rolesArray.GetType() != Json::Value::Type::Array) {
            return;
        }
        for (const auto entry: rolesArray) {
            const auto& role = entry.value();
            (void)rolesSet.insert(role);
        }
    }

    /**
     * Return a reference to the JSON object containing the key at
     * the given "path".  The path consists of JSON object keys.
     *
     * @param[in,out] rolesPermitted
     *     This tracks which roles are permitted to which operations
     *     at the current level of the JSON composition.
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
     *     This is the index of the next path element.
     *
     * @return
     *     A reference to the "descendant" JSON object containing the
     *     last key in the given sequence is returned.
     */
    const Json::Value& DescendTree(
        RolesPermitted& rolesPermitted,
        const Json::Value& root,
        const std::vector< std::string >& path,
        size_t offset = 0
    ) {
        if (offset >= path.size()) {
            return root;
        }
        const auto& key = path[offset];
        if (root.Has("data")) {
            UpdateRoles(rolesPermitted, root["meta"]);
            return DescendTree(rolesPermitted, root["data"][key], path, offset + 1);
        } else {
            return DescendTree(rolesPermitted, root[key], path, offset + 1);
        }
    }

    Json::Value ExtractDataNoMeta(
        const RolesPermitted& rolesPermitted,
        const Json::Value& root,
        const std::unordered_set< std::string >& rolesHeld
    ) {
        switch (root.GetType()) {
            case Json::Value::Type::Array: {
                if (RolePermitted(rolesPermitted.readData, rolesHeld)) {
                    auto data = Json::Array({});
                    for (const auto entry: root) {
                        auto element = ExtractData(rolesPermitted, entry.value(), rolesHeld);
                        if (element.GetType() != Json::Value::Type::Invalid) {
                            data.Add(std::move(element));
                        }
                    }
                    return data;
                } else {
                    return Json::Value();
                }
            }

            case Json::Value::Type::Object: {
                auto data = Json::Object({});
                bool empty = true;
                for (const auto entry: root) {
                    auto element = ExtractData(rolesPermitted, entry.value(), rolesHeld);
                    if (element.GetType() != Json::Value::Type::Invalid) {
                        data[entry.key()] = std::move(element);
                        empty = false;
                    }
                }
                if (
                    RolePermitted(rolesPermitted.readData, rolesHeld)
                    || !empty
                ) {
                    return data;
                } else {
                    return Json::Value();
                }
            }

            default: {
                if (RolePermitted(rolesPermitted.readData, rolesHeld)) {
                    return root;
                } else {
                    return Json::Value();
                }
            }
        }
    }

    Json::Value ExtractData(
        const RolesPermitted& rolesPermitted,
        const Json::Value& root,
        const std::unordered_set< std::string >& rolesHeld
    ) {
        if (
            (root.GetType() == Json::Value::Type::Object)
            && root.Has("data")
        ) {
            auto innerRolesPermitted = rolesPermitted;
            UpdateRoles(innerRolesPermitted, root["meta"]);
            if (RolePermitted(innerRolesPermitted.readMeta, rolesHeld)) {
                return Json::Object({
                    {"data", ExtractDataNoMeta(innerRolesPermitted, root["data"], rolesHeld)},
                    {"meta", ExtractDataNoMeta(innerRolesPermitted, root["meta"], rolesHeld)},
                });
            } else {
                return ExtractDataNoMeta(innerRolesPermitted, root["data"], rolesHeld);
            }
        }
        return ExtractDataNoMeta(rolesPermitted, root, rolesHeld);
    }

    bool RolePermitted(
        const std::unordered_set< std::string >& rolesPermitted,
        const std::unordered_set< std::string >& rolesHeld
    ) {
        if (rolesHeld.empty()) {
            return true;
        }
        for (const auto& role: rolesPermitted) {
            if (rolesHeld.find(role) != rolesHeld.end()) {
                return true;
            }
        }
        return false;
    }

    void UpdateRoles(
        RolesPermitted& rolesPermitted,
        const Json::Value& meta
    ) {
        if (meta.Has("require")) {
            const auto& require = meta["require"];
            if (require.Has("read_data")) {
                rolesPermitted.readData.clear();
                AddRoles(rolesPermitted.readData, require["read_data"]);
            }
            if (require.Has("read_meta")) {
                rolesPermitted.readMeta.clear();
                AddRoles(rolesPermitted.readMeta, require["read_meta"]);
            }
            if (require.Has("write_data")) {
                rolesPermitted.writeData.clear();
                AddRoles(rolesPermitted.writeData, require["write_data"]);
            }
            if (require.Has("write_meta")) {
                rolesPermitted.writeMeta.clear();
                AddRoles(rolesPermitted.writeMeta, require["write_meta"]);
            }
            if (require.Has("create_data")) {
                rolesPermitted.createData.clear();
                AddRoles(rolesPermitted.createData, require["create"]);
            }
            if (require.Has("delete_data")) {
                rolesPermitted.deleteData.clear();
                AddRoles(rolesPermitted.deleteData, require["delete"]);
            }
        }
        if (meta.Has("allow")) {
            const auto& allow = meta["allow"];
            if (allow.Has("read_data")) {
                AddRoles(rolesPermitted.readData, allow["read_data"]);
            }
            if (allow.Has("read_meta")) {
                AddRoles(rolesPermitted.readMeta, allow["read_meta"]);
            }
            if (allow.Has("write_data")) {
                AddRoles(rolesPermitted.readData, allow["write_data"]);
                AddRoles(rolesPermitted.writeData, allow["write_data"]);
            }
            if (allow.Has("write_meta")) {
                AddRoles(rolesPermitted.readMeta, allow["write_meta"]);
                AddRoles(rolesPermitted.writeMeta, allow["write_meta"]);
            }
            if (allow.Has("create_data")) {
                AddRoles(rolesPermitted.createData, allow["create"]);
            }
            if (allow.Has("delete_data")) {
                AddRoles(rolesPermitted.deleteData, allow["delete"]);
            }
        }
    }
}

struct Store::Impl
    : public std::enable_shared_from_this< Store::Impl >
{
    // Types

    struct Subscription {
        std::vector< std::string > path;
        std::unordered_set< std::string > rolesHeld;
        OnUpdate onUpdate;
    };

    // Properties

    SystemAbstractions::DiagnosticsSender diagnosticsSender;
    bool dirty = false;
    double minSaveInterval = 0.0;
    size_t generation = 0;
    bool mobilized = false;
    std::mutex mutex;
    double nextSaveTime = 0.0;
    int nextSaveToken = 0;
    int nextSubscriptionToken = 1;
    bool saving = false;
    Json::Value store;
    Timekeeping::Scheduler scheduler;
    std::unordered_map< int, Subscription > subscribers;

    // Constructor

    Impl()
        : diagnosticsSender("Store")
    {
    }

    // Methods

    Json::Value GetData(
        const std::vector< std::string >& path,
        const std::unordered_set< std::string >& rolesHeld
    ) {
        RolesPermitted rolesPermitted;
        const auto& root = DescendTree(rolesPermitted, store, path);
        auto data = ExtractData(rolesPermitted, root, rolesHeld);
        if (data.GetType() == Json::Value::Type::Invalid) {
            return nullptr;
        } else {
            return data;
        }
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

    std::function< void() > SubscribeToData(
        const std::vector< std::string >& path,
        const std::unordered_set< std::string >& rolesHeld,
        OnUpdate onUpdate,
        std::unique_lock< std::mutex >& lock
    ) {
        const auto subscriptionToken = nextSubscriptionToken++;
        subscribers[subscriptionToken] = { path, rolesHeld, onUpdate };
        auto data = GetData(path, rolesHeld);
        lock.unlock();
        onUpdate(std::move(data));
        lock.lock();
        std::weak_ptr< Impl > selfWeak(shared_from_this());
        return [selfWeak, subscriptionToken]{
            const auto self = selfWeak.lock();
            if (self == nullptr) {
                return;
            }
            std::lock_guard< decltype(self->mutex) > lock(self->mutex);
            (void)self->subscribers.erase(subscriptionToken);
        };
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

Json::Value Store::GetData(
    const std::vector< std::string >& path,
    const std::unordered_set< std::string >& rolesHeld
) {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    return impl_->GetData(path, rolesHeld);
}

std::function< void() > Store::SubscribeToData(
    const std::vector< std::string >& path,
    const std::unordered_set< std::string >& rolesHeld,
    std::function< void(Json::Value&& data) > onUpdate
) {
    std::unique_lock< decltype(impl_->mutex) > lock(impl_->mutex);
    return impl_->SubscribeToData(path, rolesHeld, onUpdate, lock);
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
    const auto configuration = impl_->GetData({"Configuration"}, {});
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
