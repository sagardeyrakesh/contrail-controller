/*
 * Copyright (c) 2015 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/routing-instance/route_aggregate.h"

#include <algorithm>
#include <string>

#include <boost/foreach.hpp>

#include "base/lifetime.h"
#include "base/task_annotations.h"
#include "bgp/routing-instance/path_resolver.h"
#include "bgp/routing-instance/routing_instance.h"

using std::make_pair;
using std::string;

class AggregateRouteState : public DBState {
public:
    AggregateRouteState() : contributor_(false), aggregator_(false) {
    }

    void set_aggregating_info(AggregateRoutePtr aggregator) {
        aggregating_info_ = aggregator;
        aggregator_ = true;
    }

    void reset_aggregating_info() {
        aggregating_info_ = NULL;
        aggregator_ = false;
    }

    void set_contributing_info(AggregateRoutePtr aggregator) {
        contributing_info_ = aggregator;
        contributor_ = true;
    }

    void reset_contributing_info() {
        contributing_info_ = NULL;
        contributor_ = false;
    }

    AggregateRoutePtr contributing_info() {
        return contributing_info_;
    }

    AggregateRoutePtr aggregating_info() {
        return aggregating_info_;
    }

    bool contributor() const {
        return contributor_;
    }

    bool aggregator() const {
        return aggregator_;
    }
private:
    AggregateRoutePtr contributing_info_;
    bool contributor_;
    AggregateRoutePtr aggregating_info_;
    bool aggregator_;
    DISALLOW_COPY_AND_ASSIGN(AggregateRouteState);
};

template <typename T>
class AggregateRoute : public ConditionMatch {
public:
    typedef typename T::TableT TableT;
    typedef typename T::RouteT RouteT;
    typedef typename T::PrefixT PrefixT;
    typedef typename T::AddressT AddressT;
    typedef RouteAggregator<T> AggregateRouteMgrT;
    // List of more specific routes resulted in Aggregate route PER PARTITION
    typedef std::set<BgpRoute *> RouteList;
    typedef std::vector<RouteList> ContributingRouteList;

    enum CompareResult {
        NoChange = 0,
        PrefixChange = 1,
        NexthopChange = 2,
    };

    AggregateRoute(RoutingInstance *rtinstance, AggregateRouteMgrT *manager,
        const PrefixT &aggregate_route, IpAddress nexthop);

    virtual ~AggregateRoute() {
        assert(!HasContributingRoutes());
    }

    Address::Family GetFamily() const { return manager_->GetFamily(); }
    AddressT GetAddress(IpAddress addr) const {
        return manager_->GetAddress(addr);
    }

    // Compare config and return whether cfg has updated
    CompareResult CompareAggregateRouteCfg(const AggregateRouteConfig &cfg);

    const PrefixT &aggregate_route_prefix() const {
        return aggregate_route_prefix_;
    }

    RoutingInstance *routing_instance() const {
        return routing_instance_;
    }

    BgpTable *bgp_table() const {
        return routing_instance_->GetTable(this->GetFamily());
    }

    BgpRoute *aggregate_route() const {
        return aggregate_route_;
    }

    IpAddress nexthop() const {
        return nexthop_;
    }

    bool IsMoreSpecific(BgpRoute *route) const {
        const RouteT *ip_route = static_cast<RouteT *>(route);
        const PrefixT &ip_prefix = ip_route->GetPrefix();
        if (ip_prefix.addr() != GetAddress(nexthop()) &&
            ip_prefix != aggregate_route_prefix_ &&
            ip_prefix.IsMoreSpecific(aggregate_route_prefix_)) {
            return true;
        }
        return false;
    }

    bool IsBestMatch(BgpRoute *route);

    virtual bool Match(BgpServer *server, BgpTable *table,
                       BgpRoute *route, bool deleted);

    void UpdateNexthop(IpAddress nexthop) {
        nexthop_ = nexthop;
        UpdateAggregateRoute();
    }

    void AddAggregateRoute();
    void UpdateAggregateRoute();
    void RemoveAggregateRoute();

    void set_aggregate_route(BgpRoute *aggregate);

    virtual string ToString() const {
        return (string("AggregateRoute ") +
                aggregate_route_prefix().ToString());
    }

    ContributingRouteList *contribute_route_list() {
        return &contributors_;
    }

    const ContributingRouteList &contribute_route_list() const {
        return contributors_;
    }

    bool HasContributingRoutes() const {
        BOOST_FOREACH(RouteList per_part_contributor, contribute_route_list()) {
            if (!per_part_contributor.empty()) {
                return true;
            }
        }
        return false;
    }

    void NotifyContributingRoute(BgpRoute *route) {
        DBRequest req;
        req.oper = DBRequest::DB_ENTRY_NOTIFY;
        RouteT *ip_route = static_cast<RouteT *>(route);
        const PrefixT &prefix = ip_route->GetPrefix();
        req.key.reset(new typename TableT::RequestKey(prefix, NULL));
        bgp_table()->Enqueue(&req);
    }

    AggregateRouteState *LocateRouteState(BgpRoute *route) {
        AggregateRouteState *state = static_cast<AggregateRouteState *>
            (route->GetState(bgp_table(), manager_->listener_id()));
        if (state == NULL) {
            state = new AggregateRouteState();
            route->SetState(bgp_table(), manager_->listener_id(), state);
        }
        return state;
    }

    void AddContributingRoute(BgpRoute *route) {
        contributors_[route->get_table_partition()->index()].insert(route);
        AggregateRouteState *state = LocateRouteState(route);
        state->set_contributing_info(AggregateRoutePtr(this));
        NotifyContributingRoute(route);
    }

    void ClearRouteState(BgpRoute *route, AggregateRouteState *state) {
        if (!state->aggregator() && !state->contributor()) {
            route->ClearState(bgp_table(), manager_->listener_id());
            delete state;
        }
    }

    void RemoveContributingRoute(BgpRoute *route) {
        int num_deleted =
            contributors_[route->get_table_partition()->index()].erase(route);
        AggregateRouteState *state = static_cast<AggregateRouteState *>
            (route->GetState(bgp_table(), manager_->listener_id()));
        if (state) {
            state->reset_contributing_info();
            ClearRouteState(route, state);
            NotifyContributingRoute(route);
        } else {
            assert(num_deleted != 1);
        }
    }

private:
    RoutingInstance *routing_instance_;
    AggregateRouteMgrT *manager_;
    PrefixT aggregate_route_prefix_;
    IpAddress nexthop_;
    BgpRoute *aggregate_route_;
    ContributingRouteList contributors_;

    DISALLOW_COPY_AND_ASSIGN(AggregateRoute);
};

template <typename T>
AggregateRoute<T>::AggregateRoute(RoutingInstance *rtinstance,
    AggregateRouteMgrT *manager, const PrefixT &aggregate_route,
    IpAddress nexthop)
    : routing_instance_(rtinstance),
      manager_(manager),
      aggregate_route_prefix_(aggregate_route),
      nexthop_(nexthop),
      aggregate_route_(NULL),
      contributors_(ContributingRouteList(DB::PartitionCount())) {
}

// Compare config and return whether cfg has updated
template <typename T>
typename AggregateRoute<T>::CompareResult AggregateRoute<T>::CompareAggregateRouteCfg(
    const AggregateRouteConfig &cfg) {
    AddressT address = this->GetAddress(cfg.aggregate);
    PrefixT prefix(address, cfg.prefix_length);
    if (aggregate_route_prefix_ != prefix) {
        return PrefixChange;
    }
    if (nexthop_ != cfg.nexthop) {
        return NexthopChange;
    }
    return NoChange;
}

//
// Calculate all aggregate prefixes to which the route can be contributing.
// We need to calculate the longest prefix to which this route belongs.
// E.g. routing instance is configured with 1/8, 1.1/16 and 1.1.1/24, 1.1.1.1/32
// should match 1.1.1/24. Similarly, 1.1.1/24 should be most specific to 1.1/16
// as so on
//
template <typename T>
bool AggregateRoute<T>::IsBestMatch(BgpRoute *route) {
    const RouteT *ip_route = static_cast<RouteT *>(route);
    const PrefixT &ip_prefix = ip_route->GetPrefix();
    typename RouteAggregator<T>::AggregateRouteMap::const_iterator it;
    std::set<PrefixT> prefix_list;
    for (it = manager_->aggregate_route_map().begin();
         it != manager_->aggregate_route_map().end(); ++it) {
        if (ip_prefix != it->first &&
            ip_prefix.IsMoreSpecific(it->first)) {
            prefix_list.insert(it->first);
        }
    }
    // It should match atleast one prefix
    assert(prefix_list.size());
    //
    // Longest prefix matches the aggregate prefix of current AggregateRoute
    // return true to make this route as contributing route
    // Longest prefix is the last prefix in the set
    //
    if (*(prefix_list.rbegin()) == aggregate_route_prefix_) return true;
    return false;
}

// Match function called from BgpConditionListener
// Concurrency : db::DBTable
template <typename T>
bool AggregateRoute<T>::Match(BgpServer *server, BgpTable *table,
                   BgpRoute *route, bool deleted) {
    CHECK_CONCURRENCY("db::DBTable");

    // Only interested routes
    if (!IsMoreSpecific(route)) return false;

    if (!deleted) {
        //
        // If the route is already contributing, check whether it is still
        // most specific aggregate prefix. Else remove the route as contributing
        // route. As part of the notification, route will become contributing to
        // most specific aggregate route prefix.
        //
        if (contributors_[route->get_table_partition()->index()].find(route) !=
            contributors_[route->get_table_partition()->index()].end()) {
            if (!IsBestMatch(route)) deleted = true;
        //
        // If the route is already contributing route of other aggregate prefix
        // of this bgp-table, ignore it
        //
        } else if (table->routing_instance()->IsContributingRoute(table, route))
            return false;
    }

    //
    // Consider route only if it matches most specific aggregate prefix
    // configured on the routing instance. e.g. if routing instance has following
    // prefixes configured, 1/8, 1.1/16 and 1.1.1/24, 1.1.1.1/32 should match to
    // 1.1.1/24 as most specific route.
    //
    if (!deleted && !IsBestMatch(route)) return false;

    BgpConditionListener *listener = server->condition_listener(GetFamily());
    bool state_added = listener->CheckMatchState(table, route, this);
    if (!deleted) {
        if (!state_added) {
            listener->SetMatchState(table, route, this);
            AddContributingRoute(route);
        }
    } else {
        if (!state_added) {
            // Not seen ADD ignore DELETE
            return false;
        }
        RemoveContributingRoute(route);
        listener->RemoveMatchState(table, route, this);
    }

    manager_->EvaluateRouteAggregate(this);
    return true;
}

// AddAggregateRoute
template <typename T>
void AggregateRoute<T>::AddAggregateRoute() {
    CHECK_CONCURRENCY("bgp::RouteAggregation");

    RouteT rt_key(aggregate_route_prefix());
    DBTablePartition *partition =
       static_cast<DBTablePartition *>(bgp_table()->GetTablePartition(&rt_key));
    BgpRoute *aggregate_route =
        static_cast<BgpRoute *>(partition->Find(&rt_key));

    if (aggregate_route == NULL) {
        aggregate_route = new RouteT(aggregate_route_prefix());
        partition->Add(aggregate_route);
    } else {
        aggregate_route->ClearDelete();
    }

    BgpPath *existing_path = aggregate_route->FindPath(BgpPath::Aggregation, 0);
    assert(existing_path == NULL);

    BgpAttrSpec attrs;
    BgpAttrNextHop attr_nexthop(this->GetAddress(nexthop()));
    attrs.push_back(&attr_nexthop);
    BgpAttrPtr attr = routing_instance()->server()->attr_db()->Locate(attrs);
    BgpPath *new_path = new BgpPath(BgpPath::Aggregation,
                                    attr.get(), BgpPath::ResolveNexthop, 0);
    bgp_table()->path_resolver()->StartPathResolution(partition->index(),
                                                     new_path, aggregate_route);
    aggregate_route->InsertPath(new_path);
    partition->Notify(aggregate_route);
    set_aggregate_route(aggregate_route);
}

// UpdateAggregateRoute
template <typename T>
void AggregateRoute<T>::UpdateAggregateRoute() {
    CHECK_CONCURRENCY("bgp::Config");

    if (aggregate_route_ == NULL) return;

    DBTablePartition *partition = static_cast<DBTablePartition *>
        (bgp_table()->GetTablePartition(aggregate_route_));

    aggregate_route_->ClearDelete();

    BgpPath *existing_path = aggregate_route_->FindPath(BgpPath::Aggregation, 0);
    if (existing_path)
        bgp_table()->path_resolver()->StopPathResolution(partition->index(),
                                                         existing_path);
    aggregate_route_->RemovePath(BgpPath::Aggregation);

    BgpAttrSpec attrs;
    BgpAttrNextHop attr_nexthop(this->GetAddress(nexthop()));
    attrs.push_back(&attr_nexthop);
    BgpAttrPtr attr = routing_instance()->server()->attr_db()->Locate(attrs);
    BgpPath *new_path = new BgpPath(BgpPath::Aggregation,
                                    attr.get(), BgpPath::ResolveNexthop, 0);
    bgp_table()->path_resolver()->StartPathResolution(partition->index(),
                                                    new_path, aggregate_route_);
    aggregate_route_->InsertPath(new_path);

    partition->Notify(aggregate_route_);
}

// RemoveAggregateRoute
template <typename T>
void AggregateRoute<T>::RemoveAggregateRoute() {
    CHECK_CONCURRENCY("bgp::RouteAggregation");
    BgpRoute *aggregate_route = aggregate_route_;
    if (!aggregate_route) return;

    DBTablePartition *partition = static_cast<DBTablePartition *>
        (bgp_table()->GetTablePartition(aggregate_route_));

    BgpPath *existing_path =
        aggregate_route->FindPath(BgpPath::Aggregation, 0);
    assert(existing_path != NULL);

    bgp_table()->path_resolver()->StopPathResolution(partition->index(),
                                                     existing_path);
    aggregate_route->RemovePath(BgpPath::Aggregation);

    if (!aggregate_route->BestPath()) {
        partition->Delete(aggregate_route);
    } else {
        partition->Notify(aggregate_route);
    }
    set_aggregate_route(NULL);
}

template <typename T>
void AggregateRoute<T>::set_aggregate_route(BgpRoute *aggregate) {
    if (aggregate) {
        assert(aggregate_route_ == NULL);
        AggregateRouteState *state = LocateRouteState(aggregate);
        state->set_aggregating_info(AggregateRoutePtr(this));
    } else {
        assert(aggregate_route_ != NULL);
        AggregateRouteState *state = static_cast<AggregateRouteState *>
            (aggregate_route_->GetState(bgp_table(), manager_->listener_id()));
        assert(state);
        state->reset_aggregating_info();
        ClearRouteState(aggregate_route_, state);
    }
    aggregate_route_ = aggregate;
}

template <typename T>
class RouteAggregator<T>::DeleteActor : public LifetimeActor {
public:
    explicit DeleteActor(RouteAggregator *aggregator) :
    LifetimeActor(aggregator->routing_instance()->server()->lifetime_manager()),
    aggregator_(aggregator) {
    }
    virtual ~DeleteActor() {
    }

    virtual bool MayDelete() const {
        return aggregator_->MayDelete();
    }

    virtual void Destroy() {
        aggregator_->routing_instance()->DestroyRouteAggregator(
                                                    aggregator_->GetFamily());
    }

private:
    RouteAggregator *aggregator_;
};

template <typename T>
RouteAggregator<T>::RouteAggregator(RoutingInstance *rtinstance)
  : rtinstance_(rtinstance),
    condition_listener_(rtinstance_->server()->condition_listener(GetFamily())),
    listener_id_(DBTableBase::kInvalidId),
    add_remove_contributing_route_trigger_(new TaskTrigger(
        boost::bind(&RouteAggregator::ProcessRouteAggregateUpdate, this),
        TaskScheduler::GetInstance()->GetTaskId("bgp::RouteAggregation"),
        0)),
    resolve_trigger_(new TaskTrigger(
        boost::bind(&RouteAggregator::ProcessUnregisterResolveConfig, this),
        TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0)),
    deleter_(new DeleteActor(this)),
    instance_delete_ref_(this, rtinstance->deleter()) {
}

template <typename T>
RouteAggregator<T>::~RouteAggregator() {
    if (listener_id_ != DBTableBase::kInvalidId)
        bgp_table()->Unregister(listener_id_);
    listener_id_ = DBTableBase::kInvalidId;
}

template <typename T>
void RouteAggregator<T>::ProcessAggregateRouteConfig() {
    CHECK_CONCURRENCY("bgp::Config");
    const BgpInstanceConfig::AggregateRouteList &list =
        routing_instance()->config()->aggregate_routes(GetFamily());
    typedef BgpInstanceConfig::AggregateRouteList::const_iterator iterator_t;
    for (iterator_t iter = list.begin(); iter != list.end(); ++iter) {
        LocateAggregateRoutePrefix(*iter);
    }
}

bool CompareAggregateRouteConfig(const AggregateRouteConfig &lhs,
                                 const AggregateRouteConfig &rhs) {
    BOOL_KEY_COMPARE(lhs.aggregate, rhs.aggregate);
    BOOL_KEY_COMPARE(lhs.prefix_length, rhs.prefix_length);
    return false;
}

template <typename T>
void RouteAggregator<T>::UpdateAggregateRouteConfig() {
    CHECK_CONCURRENCY("bgp::Config");
    typedef BgpInstanceConfig::AggregateRouteList AggregateRouteList;
    AggregateRouteList aggregate_route_list =
        routing_instance()->config()->aggregate_routes(GetFamily());

    sort(aggregate_route_list.begin(), aggregate_route_list.end(),
              CompareAggregateRouteConfig);

    // TODO templatize the sync operation
    AggregateRouteList::const_iterator aggregate_route_cfg_it =
            aggregate_route_list.begin();
    typename AggregateRouteMap::iterator oper_it = aggregate_route_map_.begin();

    while ((aggregate_route_cfg_it != aggregate_route_list.end()) &&
           (oper_it != aggregate_route_map_.end())) {
        AddressT address = this->GetAddress(aggregate_route_cfg_it->aggregate);
        PrefixT aggregate_route_prefix(address,
                                       aggregate_route_cfg_it->prefix_length);
        if (aggregate_route_prefix < oper_it->first) {
            LocateAggregateRoutePrefix(*aggregate_route_cfg_it);
            aggregate_route_cfg_it++;
        } else if (aggregate_route_prefix > oper_it->first) {
            RemoveAggregateRoutePrefix(oper_it->first);
            oper_it++;
        } else {
            LocateAggregateRoutePrefix(*aggregate_route_cfg_it);
            aggregate_route_cfg_it++;
            oper_it++;
        }
    }

    for (; oper_it != aggregate_route_map_.end(); oper_it++) {
        RemoveAggregateRoutePrefix(oper_it->first);
    }
    for (; aggregate_route_cfg_it != aggregate_route_list.end();
         aggregate_route_cfg_it++) {
        LocateAggregateRoutePrefix(*aggregate_route_cfg_it);
    }
}

template <typename T>
void RouteAggregator<T>::FlushAggregateRouteConfig() {
    CHECK_CONCURRENCY("bgp::Config");
    for (typename AggregateRouteMap::iterator it = aggregate_route_map_.begin();
         it != aggregate_route_map_.end(); it++) {
        RemoveAggregateRoutePrefix(it->first);
    }
}

template <>
Address::Family RouteAggregator<AggregateInetRoute>::GetFamily() const {
    return Address::INET;
}

template <>
Address::Family RouteAggregator<AggregateInet6Route>::GetFamily() const {
    return Address::INET6;
}

template <>
Ip4Address RouteAggregator<AggregateInetRoute>::GetAddress(IpAddress addr)
    const {
    assert(addr.is_v4());
    return addr.to_v4();
}

template <>
Ip6Address RouteAggregator<AggregateInet6Route>::GetAddress(IpAddress addr)
    const {
    assert(addr.is_v6());
    return addr.to_v6();
}

template <typename T>
BgpTable *RouteAggregator<T>::bgp_table() const {
    return rtinstance_->GetTable(GetFamily());
}

template <typename T>
void RouteAggregator<T>::Initialize() {
    // Register to the table before adding first match condition
    listener_id_ = bgp_table()->Register(
         boost::bind(&RouteAggregator::RouteListener, this, _1, _2),
         "RouteAggregator");
}

template <typename T>
bool RouteAggregator<T>::MayDelete() const {
    if (!aggregate_route_map_.empty())
        return false;
    if (!update_aggregate_list_.empty())
        return false;
    if (!unregister_aggregate_list_.empty())
        return false;
    return true;
}

// Cascade delete from RoutingInstance delete_ref to self.
template <typename T>
void RouteAggregator<T>::ManagedDelete() {
    deleter_->Delete();
}

// Attempt to enqueue a delete for the RouteAggregator.
template <typename T>
void RouteAggregator<T>::RetryDelete() {
    if (!deleter_->IsDeleted())
        return;
    deleter_->RetryDelete();
}

template <typename T>
void RouteAggregator<T>::EvaluateRouteAggregate(AggregateRoutePtr entry) {
    tbb::mutex::scoped_lock lock(mutex_);
    update_aggregate_list_.insert(entry);
    add_remove_contributing_route_trigger_->Set();
}

template <typename T>
void RouteAggregator<T>::UnregisterAndResolveRouteAggregate(
                                                    AggregateRoutePtr entry) {
    tbb::mutex::scoped_lock lock(mutex_);
    unregister_aggregate_list_.insert(entry);
    resolve_trigger_->Set();
}

template <typename T>
bool RouteAggregator<T>::IsAggregateRoute(const BgpRoute *route) const {
    AggregateRouteState *state = static_cast<AggregateRouteState *>
        (route->GetState(bgp_table(), listener_id()));
    if (state) {
        return (state->contributor() == false);
    }
    return false;
}

template <typename T>
bool RouteAggregator<T>::IsContributingRoute(const BgpRoute *route) const {
    AggregateRouteState *state = static_cast<AggregateRouteState *>
        (route->GetState(bgp_table(), listener_id()));
    if (state) {
        return state->contributor();
    }
    return false;
}

template <typename T>
void RouteAggregator<T>::LocateAggregateRoutePrefix(const AggregateRouteConfig
                                                    &cfg) {
    CHECK_CONCURRENCY("bgp::Config");
    AddressT address = this->GetAddress(cfg.aggregate);
    PrefixT prefix(address, cfg.prefix_length);

    // Verify whether the entry already exists
    typename AggregateRouteMap::iterator it = aggregate_route_map_.find(prefix);
    if (it != aggregate_route_map_.end()) {
        // Wait for the delete complete cb
        if (it->second->deleted()) return;

        AggregateRouteT *match =
            static_cast<AggregateRouteT *>(it->second.get());
        // Check whether the config has got updated
        typename AggregateRouteT::CompareResult change =
            match->CompareAggregateRouteCfg(cfg);
        // No change..
        if (change == AggregateRouteT::NoChange) return;

        if (change == AggregateRouteT::NexthopChange)
            match->UpdateNexthop(cfg.nexthop);
        return;
    }

    AggregateRouteT *match =
        new AggregateRouteT(routing_instance(), this, prefix, cfg.nexthop);
    AggregateRoutePtr aggregate_route_match = AggregateRoutePtr(match);
    aggregate_route_map_.insert(make_pair(prefix, aggregate_route_match));

    condition_listener_->AddMatchCondition(match->bgp_table(),
           aggregate_route_match.get(), BgpConditionListener::RequestDoneCb());
    return;
}

template <typename T>
void RouteAggregator<T>::RemoveAggregateRoutePrefix(const PrefixT &aggregate) {
    CHECK_CONCURRENCY("bgp::Config");
    typename AggregateRouteMap::iterator it =
        aggregate_route_map_.find(aggregate);
    if (it == aggregate_route_map_.end()) return;
    if (it->second->deleted()) return;

    BgpConditionListener::RequestDoneCb callback =
        boost::bind(&RouteAggregator::StopAggregateRouteDone, this, _1, _2);

    AggregateRouteT *match = static_cast<AggregateRouteT *>(it->second.get());
    condition_listener_->RemoveMatchCondition(match->bgp_table(),
                                              match, callback);
}

template <typename T>
void RouteAggregator<T>::StopAggregateRouteDone(BgpTable *table,
                                             ConditionMatch *info) {
    CHECK_CONCURRENCY("db::DBTable");
    UnregisterAndResolveRouteAggregate(info);
    return;
}

template <typename T>
bool RouteAggregator<T>::ProcessUnregisterResolveConfig() {
    CHECK_CONCURRENCY("bgp::Config");

    for (AggregateRouteProcessList::iterator
         it = unregister_aggregate_list_.begin();
         it != unregister_aggregate_list_.end(); ++it) {
        AggregateRoutePtr aggregate = *it;
        AggregateRouteT *info = static_cast<AggregateRouteT *>(aggregate.get());
        aggregate_route_map_.erase(info->aggregate_route_prefix());
        condition_listener_->UnregisterMatchCondition(info->bgp_table(), info);
    }

    unregister_aggregate_list_.clear();

    if (!routing_instance()->deleted() && routing_instance()->config())
        ProcessAggregateRouteConfig();

    if (MayDelete()) RetryDelete();
    return true;
}

template <typename T>
bool RouteAggregator<T>::ProcessRouteAggregateUpdate() {
    CHECK_CONCURRENCY("bgp::RouteAggregation");

    for (AggregateRouteProcessList::iterator
         it = update_aggregate_list_.begin();
         it != update_aggregate_list_.end(); ++it) {
        AggregateRouteT *aggregate = static_cast<AggregateRouteT *>(it->get());
        if (aggregate->aggregate_route()) {
            if (!aggregate->HasContributingRoutes())
                aggregate->RemoveAggregateRoute();
        } else {
            if (aggregate->HasContributingRoutes())
                aggregate->AddAggregateRoute();
        }
    }

    update_aggregate_list_.clear();

    if (MayDelete()) RetryDelete();
    return true;
}

// Need this to store the aggregate info in aggregated route as DBState
template <typename T>
bool RouteAggregator<T>::RouteListener(DBTablePartBase *root,
                                       DBEntryBase *entry) {
    return true;
}

// Enable/Disable task triggers
template <typename T>
void RouteAggregator<T>::DisableRouteAggregateUpdate() {
    add_remove_contributing_route_trigger_->set_disable();
}

template <typename T>
void RouteAggregator<T>::EnableRouteAggregateUpdate() {
    add_remove_contributing_route_trigger_->set_enable();
}

template <typename T>
size_t RouteAggregator<T>::GetUpdateAggregateListSize() const{
    return update_aggregate_list_.size();
}

template <typename T>
void RouteAggregator<T>::DisableUnregResolveTask(){
    resolve_trigger_->set_disable();
}

template <typename T>
void RouteAggregator<T>::EnableUnregResolveTask(){
    resolve_trigger_->set_enable();
}

template <typename T>
size_t RouteAggregator<T>::GetUnregResolveListSize() const{
    return unregister_aggregate_list_.size();
}

// Explicit instantiation of RouteAggregator for INET and INET6.
template class RouteAggregator<AggregateInetRoute>;
template class RouteAggregator<AggregateInet6Route>;