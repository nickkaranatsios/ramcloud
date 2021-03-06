/* Copyright (c) 2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "CoordinatorServerList.h"
#include "CoordinatorService.h"
#include "Logger.h"
#include "MasterClient.h"
#include "ShortMacros.h"
#include "TableManager.h"

namespace RAMCloud {

/**
 * Construct a TableManager.
 */
TableManager::TableManager(Context* context)
    : mutex()
    , context(context)
    , logIdLargestTableId(NO_ID)
    , map()
    , nextTableId(1)
    , nextTableMasterIdx(0)
    , tables()
    , tablesLogIds()
{
    context->tableManager = this;
}

/**
 * Destructor for TableManager.
 */
TableManager::~TableManager(){}

//////////////////////////////////////////////////////////////////////
// TableManager Public Methods
//////////////////////////////////////////////////////////////////////

/**
 * Create a table with the given name.
 * 
 * \param name
 *      Name for the table to be created.
 * \param serverSpan
 *      Number of servers across which this table should be split during
 *      creation.
 * 
 * \return
 *      tableId of the table created.
 * 
 * \throw TableExists
 *      If trying to create a table that already exists.
 */
uint64_t
TableManager::createTable(const char* name, uint32_t serverSpan)
{
    Lock lock(mutex);

    return CreateTable(*this, lock, name, uint64_t(), serverSpan).execute();
}

/**
 * Returns a protocol-buffer-like debug string listing the details of each of
 * the tablets currently in the tablet map.
 */
string
TableManager::debugString() const
{
    Lock lock(mutex);
    std::stringstream result;
    for (auto it = map.begin(); it != map.end(); ++it)  {
        if (it != map.begin())
            result << " ";
        const auto& tablet = *it;
        const char* status = "NORMAL";
        if (tablet.status != Tablet::NORMAL)
            status = "RECOVERING";
        result << "Tablet { tableId: " << tablet.tableId
               << " startKeyHash: " << tablet.startKeyHash
               << " endKeyHash: " << tablet.endKeyHash
               << " serverId: " << tablet.serverId.toString().c_str()
               << " status: " << status
               << " ctime: " << tablet.ctime.getSegmentId()
               << ", " << tablet.ctime.getSegmentOffset() <<  " }";
    }
    return result.str();
}

/**
 * Drop the table with the given name.
 * 
 * \param name
 *      Name to identify the table that is to be dropped.
 * 
 * \throw NoSuchTable
 *      If name does not identify a table currently in the tables.
 */
void
TableManager::dropTable(const char* name)
{
    Lock lock(mutex);

    return DropTable(*this, lock, name).execute();
}

/**
 * Get the tableId of the table with the given name.
 * 
 * \param name
 *      Name to identify the table.
 * 
 * \return
 *      tableId of the table whose name is given.
 * 
 * \throw NoSuchTable
 *      If name does not identify a table currently in the tables.
 */
uint64_t
TableManager::getTableId(const char* name)
{
    Lock lock(mutex);
    Tables::iterator it(tables.find(name));
    if (it == tables.end()) {
        throw NoSuchTable(HERE);
    }
    return it->second;
}

/**
 * Update the status of all the Tablets in the tablet map that are on a
 * specific server as recovering.
 * Copies of the details about the affected Tablets are returned.
 *
 * \param serverId
 *      Table id of the table whose tablets status should be changed to
 *      \a status.
 * \return
 *      List of copies of all the Tablets in the tablet map which are owned
 *      by the server indicated by \a serverId.
 */
vector<Tablet>
TableManager::markAllTabletsRecovering(ServerId serverId)
{
    Lock lock(mutex);
    vector<Tablet> results;
    foreach (Tablet& tablet, map) {
        if (tablet.serverId == serverId) {
            tablet.status = Tablet::RECOVERING;
            results.push_back(tablet);
        }
    }
    return results;
}

/**
 * Switch ownership of the tablet and alert the new owner that it may
 * begin servicing requests on that tablet.
 * 
 * \param newOwner
 *      ServerId of the server that will own this tablet at the end of
 *      the operation.
 * \param tableId
 *      Table id of the tablet whose ownership is being reassigned.
 * \param startKeyHash
 *      First key hash that is part of the range of key hashes for the tablet.
 * \param endKeyHash
 *      Last key hash that is part of the range of key hashes for the tablet.
 * \param ctimeSegmentId
 *      ServerId of the log head before migration.
 * \param ctimeSegmentOffset
 *      Offset in log head before migration.
 * 
 * \throw NoSuchTablet
 *      If the arguments do not identify a tablet currently in the tablet map.
 */
void
TableManager::reassignTabletOwnership(
        ServerId newOwner, uint64_t tableId,
        uint64_t startKeyHash, uint64_t endKeyHash,
        uint64_t ctimeSegmentId, uint64_t ctimeSegmentOffset)
{
    Lock lock(mutex);
    // Could throw TableManager::NoSuchTablet exception
    Tablet tablet = getTablet(lock, tableId, startKeyHash, endKeyHash);
    LOG(NOTICE, "Reassigning tablet [0x%lx,0x%lx] in tableId %lu "
        "from %s to %s",
        startKeyHash, endKeyHash, tableId,
        context->coordinatorServerList->toString(tablet.serverId).c_str(),
        context->coordinatorServerList->toString(newOwner).c_str());

    // Get current head of log to preclude all previous data in the log
    // from being considered part of this tablet.
    Log::Position headOfLogAtCreation(ctimeSegmentId,
                                      ctimeSegmentOffset);

    // Could throw TableManager::NoSuchTablet exception
    modifyTablet(lock, tableId, startKeyHash, endKeyHash,
                 newOwner, Tablet::NORMAL, headOfLogAtCreation);

    // TODO(rumble/slaughter) If we fail to alert the new owner we could
    //      get stuck in limbo. What should we do? Retry? Fail the
    //      server and recover it? Can't return to the old master if we
    //      reply early...
    MasterClient::takeTabletOwnership(context, newOwner, tableId,
                                      startKeyHash, endKeyHash);
}

/**
 * Copy the contents of the tablet map into a protocol buffer, \a tablets,
 * suitable for sending across the wire to servers.
 *
 * \param serverList
 *      The single instance of the AbstractServerList. Used to fill in
 *      the service_locator field of entries in \a tablets;
 * \param tablets
 *      Protocol buffer to which entries are added representing each of
 *      the tablets in the tablet map.
 */
void
TableManager::serialize(AbstractServerList* serverList,
                        ProtoBuf::Tablets* tablets) const
{
    Lock lock(mutex);
    foreach (const auto& tablet, map) {
        ProtoBuf::Tablets::Tablet& entry(*tablets->add_tablet());
        tablet.serialize(entry);
        try {
            string locator = serverList->getLocator(tablet.serverId);
            entry.set_service_locator(locator);
        } catch (const Exception& e) {
            LOG(NOTICE, "Server id (%s) in tablet map no longer in server "
                "list; sending empty locator for entry",
                tablet.serverId.toString().c_str());
        }
    }
}

/**
 * Split a Tablet in the tablet map into two disjoint Tablets at a specific
 * key hash. Check if the split already exists, in which case, just return.
 * After a split, inform the master to split the tablet.
 * 
 * \param name
 *      Name of the table that contains the tablet to be split.
 * \param splitKeyHash
 *      Key hash to used to partition the tablet into two. Keys less than
 *      \a splitKeyHash belong to one Tablet, keys greater than or equal to
 *      \a splitKeyHash belong to the other.
 * 
 * \throw NoSuchTable
 *      If name does not identify a table currently in the tables.
 */
void
TableManager::splitTablet(const char* name,
                          uint64_t splitKeyHash)
{
    Lock lock(mutex);
    SplitTablet(*this, lock, name, splitKeyHash).execute();
}

/**
 * Used by MasterRecoveryManager after recovery for a tablet has successfully
 * completed to inform coordinator about the new master for the tablet.
 * 
 * \param tableId
 *      Table id of the tablet.
 * \param startKeyHash
 *      First key hash that is part of range of key hashes for the tablet.
 * \param endKeyHash
 *      Last key hash that is part of range of key hashes for the tablet.
 * \param serverId
 *      Tablet is updated to indicate that it is owned by \a serverId.
 * \param ctime
 *      Tablet is updated with this ctime indicating any object earlier
 *      than \a ctime in its log cannot contain objects belonging to it.
 * \throw NoSuchTablet
 *      If the arguments do not identify a tablet currently in the tablet map.
 */
void
TableManager::tabletRecovered(
        uint64_t tableId, uint64_t startKeyHash, uint64_t endKeyHash,
        ServerId serverId, Log::Position ctime)
{
    Lock lock(mutex);
    TabletRecovered(*this, lock, tableId, startKeyHash, endKeyHash,
                    serverId, ctime).execute();
}

//////////////////////////////////////////////////////////////////////
// TableManager Recovery Methods
//////////////////////////////////////////////////////////////////////

/**
 * During coordinator recovery, add local metadata for a table that had
 * already been successfully created.
 *
 * \param state
 *      The ProtoBuf that encapsulates the information about the table.
 * \param entryId
 *      The entry id of the LogCabin entry corresponding to the state.
 */
void
TableManager::recoverAliveTable(
    ProtoBuf::TableInformation* state, EntryId entryId)
{
    Lock lock(mutex);
    LOG(DEBUG, "TableManager::recoverCreateTable()");

    nextTableId = state->table_id() + 1;

    uint64_t tableId = state->table_id();
    tables[state->name()] = tableId;
    tablesLogIds[tableId].tableInfoLogId = entryId;

    for (uint32_t i = 0; i < state->server_span(); i++) {
        const ProtoBuf::TableInformation::TabletInfo* tabletInfo =
                &state->tablet_info(i);

        uint64_t firstKeyHash = tabletInfo->start_key_hash();
        uint64_t lastKeyHash = tabletInfo->end_key_hash();
        ServerId computedMasterId = ServerId(tabletInfo->master_id());
        Log::Position headOfLog(tabletInfo->ctime_log_head_id(),
                tabletInfo->ctime_log_head_id());

        // Create tablet map entry.
        addTablet(lock, {tableId, firstKeyHash, lastKeyHash,
                         computedMasterId, Tablet::NORMAL, headOfLog});
    }
}

/**
 * During coordinator recovery, complete a createTable operation that
 * had already been started.
 *
 * \param state
 *      The ProtoBuf that encapsulates the information about table
 *      being created.
 * \param entryId
 *      The entry id of the LogCabin entry corresponding to the state.
 */
void
TableManager::recoverCreateTable(
    ProtoBuf::TableInformation* state, EntryId entryId)
{
    Lock lock(mutex);
    LOG(DEBUG, "TableManager::recoverCreateTable()");
    nextTableId = state->table_id() + 1;
    CreateTable(*this, lock,
                state->name().c_str(),
                state->table_id(),
                state->server_span(),
                *state).complete(entryId);
}

/**
 * During coordinator recovery, complete a dropTable operation that
 * had already been started.
 *
 * \param state
 *      The ProtoBuf that encapsulates the information about table
 *      being dropped.
 * \param entryId
 *      The entry id of the LogCabin entry corresponding to the state.
 */
void
TableManager::recoverDropTable(
    ProtoBuf::TableDrop* state, EntryId entryId)
{
    Lock lock(mutex);
    LOG(DEBUG, "TableManager::recoverDropTable()");
    DropTable(*this, lock,
              state->name().c_str()).complete(entryId);
}

/**
 * During coordinator recovery, recover information about largest table id.
 * This entry is encountered if the table with the largest table id was
 * deleted, and hence the largest table id would not be recoverable by simply
 * replaying the TableInformation protobuf corresponding to CreateTable
 * or AliveTable entry types.
 *
 * \param state
 *      The ProtoBuf that encapsulates the information about table
 *      being dropped.
 * \param entryId
 *      The entry id of the LogCabin entry corresponding to the state.
 */
void
TableManager::recoverLargestTableId(
    ProtoBuf::LargestTableId* state, EntryId entryId)
{
    Lock lock(mutex);
    LOG(DEBUG, "TableManager::recoverLargestTableId()");
    nextTableId = state->table_id() + 1;
    logIdLargestTableId = entryId;
}

/**
 * During coordinator recovery, complete a splitTablet operation that
 * had already been started.
 *
 * \param state
 *      The ProtoBuf that encapsulates the information about the tablet
 *      that was being split.
 * \param entryId
 *      The entry id of the LogCabin entry corresponding to the state.
 */
void
TableManager::recoverSplitTablet(
    ProtoBuf::SplitTablet* state, EntryId entryId)
{
    Lock lock(mutex);
    LOG(DEBUG, "TableManager::recoverSplitTablet()");
    SplitTablet(*this, lock,
                state->name().c_str(),
                state->split_key_hash()).complete(entryId);
}

/**
 * During coordinator recovery, complete a tabletRecovered operation that
 * had already been started.
 *
 * \param state
 *      The ProtoBuf that encapsulates the information about the tablet
 *      that was recovered during a master recovery.
 * \param entryId
 *      The entry id of the LogCabin entry corresponding to the state.
 */
void
TableManager::recoverTabletRecovered(
    ProtoBuf::TabletRecovered* state, EntryId entryId)
{
    Lock lock(mutex);
    LOG(DEBUG, "TableManager::recoverTabletRecovered()");
    TabletRecovered(*this, lock,
                    state->table_id(),
                    state->start_key_hash(),
                    state->end_key_hash(),
                    ServerId(state->server_id()),
                    Log::Position(state->ctime_log_head_id(),
                            state->ctime_log_head_offset())).complete(entryId);
}

/////////////////////////////////////////////////////////////////////////////
// TableManager Private Methods
/////////////////////////////////////////////////////////////////////////////

/**
 * Do everything needed to execute the CreateTable operation.
 * Do any processing required before logging the state
 * in LogCabin, log the state in LogCabin, then call #complete().
 * 
 * \return
 *      The tableId assigned to the table created.
 */
uint64_t
TableManager::CreateTable::execute()
{
    CoordinatorService *coordService = tm.context->coordinatorService;
    RuntimeOptions *runtimeOptions = NULL;
    // context->coordinatorService can be NULL in test mode
    if (coordService) {
        runtimeOptions = coordService->getRuntimeOptionsFromCoordinator();
        runtimeOptions->checkAndCrashCoordinator("create_1");
    }

    if (tm.tables.find(name) != tm.tables.end())
        throw TableExists(HERE);
    tableId = tm.nextTableId++;

    LOG(NOTICE, "Creating table '%s' with id %lu", name, tableId);

    if (serverSpan == 0)
        serverSpan = 1;

    state.set_entry_type("CreateTable");
    state.set_name(name);
    state.set_table_id(tableId);
    state.set_server_span(serverSpan);

    for (uint32_t i = 0; i < serverSpan; i++) {
        uint64_t firstKeyHash = i * (~0UL / serverSpan);
        if (i != 0)
            firstKeyHash++;
        uint64_t lastKeyHash = (i + 1) * (~0UL / serverSpan);
        if (i == serverSpan - 1)
            lastKeyHash = ~0UL;

        // Find the next master in the list.
        CoordinatorServerList::Entry master;
        while (true) {
            size_t masterIdx = tm.nextTableMasterIdx++ %
                               tm.context->coordinatorServerList->size();
            CoordinatorServerList::Entry entry;
            try {
                entry = (*tm.context->coordinatorServerList)[masterIdx];
                if (entry.isMaster()) {
                    master = entry;
                    break;
                }
            } catch (ServerListException& e) {
                continue;
            }
        }

        // add to local tablet map

        // headOfLog is actually supposed to be the current log head on master.
        // Only entries >= this can be part of the tablet.
        // For a new table that is being created, using a value of (0,0)
        // suffices for safety, and reduces the number of calls to the masters.
        Log::Position headOfLog(0, 0);

        ProtoBuf::TableInformation::TabletInfo&
                tablet(*state.add_tablet_info());
        tablet.set_start_key_hash(firstKeyHash);
        tablet.set_end_key_hash(lastKeyHash);
        tablet.set_master_id(master.serverId.getId());
        tablet.set_ctime_log_head_id(headOfLog.getSegmentId());
        tablet.set_ctime_log_head_offset(headOfLog.getSegmentOffset());
    }

    vector<EntryId> invalidates;

    // If there was a LargestTableId log entry appended to LogCabin, invalidate
    // it as it is no longer needed. The tableId for this table being created
    // has to be strictly larger than the one indicated in the LargestTableId
    // entry, hence there is no use for it anymore.
    if (tm.logIdLargestTableId != NO_ID) {
        invalidates.push_back(tm.logIdLargestTableId);
        tm.logIdLargestTableId = NO_ID;
    }

    EntryId entryId =
        tm.context->logCabinHelper->appendProtoBuf(
            *tm.context->expectedEntryId, state, invalidates);
    tm.setTableInfoLogId(lock, tableId, entryId);
    LOG(DEBUG, "LogCabin: CreateTable entryId: %lu", entryId);

    if (runtimeOptions)
        runtimeOptions->checkAndCrashCoordinator("create_2");

    return complete(entryId);
}

/**
 * Complete the CreateTable operation after its state has been
 * logged in LogCabin.
 * This is called internally by #execute() in case of normal operation
 * (which is in turn called by #createTable()), and
 * directly for coordinator recovery (by #recoverCreateTable()).
 *
 * \param entryId
 *      The entry id of the LogCabin entry corresponding to the state
 *      of the operation to be completed.
 * 
 * \return
 *      The tableId assigned to the table created.
 */
uint64_t
TableManager::CreateTable::complete(EntryId entryId)
{
    tm.tables[name] = tableId;
    tm.tablesLogIds[tableId].tableInfoLogId = entryId;

    for (uint32_t i = 0; i < serverSpan; i++) {
        const ProtoBuf::TableInformation::TabletInfo* tabletInfo =
                &state.tablet_info(i);

        uint64_t firstKeyHash = tabletInfo->start_key_hash();
        uint64_t lastKeyHash = tabletInfo->end_key_hash();
        ServerId computedMasterId = ServerId(tabletInfo->master_id());
        Log::Position headOfLog(tabletInfo->ctime_log_head_id(),
                tabletInfo->ctime_log_head_id());

        // Create tablet map entry.
        tm.addTablet(lock, {tableId, firstKeyHash, lastKeyHash,
                            computedMasterId, Tablet::NORMAL, headOfLog});

        try {
            CoordinatorServerList::Entry computedMaster =
                        (*tm.context->coordinatorServerList)[computedMasterId];
            assert(computedMaster.isMaster());

            // Inform the master if it is up.
            MasterClient::takeTabletOwnership(tm.context, computedMasterId,
                        tableId, firstKeyHash, lastKeyHash);
        } catch (ServerListException& e) {
            // If the computer master doesn't exist anymore, that means that
            // it has crashed. Its recovery may or may not have been started
            // yet.
            // However, when the master recovery contacts the coordinator
            // to get the tablet map, it will be stalled till the end of
            // the create table completion since this operation is currently
            // holding the TableManager lock.
            // Thus, when we add the entry for the current tablet to the
            // tablet map, it will be recovered as a part of the crashed master
            // recovery, whenever that recovery contacts the coordinator.
            LOG(NOTICE, "Master that was computed doesn't exist anymore.");
        }

        LOG(NOTICE,
            "Assigned tablet [0x%lx,0x%lx] in table '%s' (id %lu) to %s",
            firstKeyHash, lastKeyHash, name, tableId,
            tm.context->coordinatorServerList->toString(
                        computedMasterId).c_str());
    }

    CoordinatorService *coordService = tm.context->coordinatorService;
    RuntimeOptions *runtimeOptions = NULL;
    // context->coordinatorService can be NULL in test mode
    if (coordService) {
        runtimeOptions = coordService->getRuntimeOptionsFromCoordinator();
        runtimeOptions->checkAndCrashCoordinator("create_3");
    }

    state.set_entry_type("AliveTable");
    EntryId newEntryId = tm.context->logCabinHelper->appendProtoBuf(
            *tm.context->expectedEntryId, state, vector<EntryId>({entryId}));
    tm.setTableInfoLogId(lock, tableId, newEntryId);
    LOG(DEBUG, "LogCabin: AliveTable entryId: %lu", newEntryId);

    if (runtimeOptions)
        runtimeOptions->checkAndCrashCoordinator("create_4");

    return tableId;
}

void
TableManager::DropTable::execute()
{
    Tables::iterator it = tm.tables.find(name);
    if (it == tm.tables.end())
        return;

    ProtoBuf::TableDrop state;
    state.set_entry_type("DropTable");
    state.set_name(name);

    CoordinatorService *coordService = tm.context->coordinatorService;
    RuntimeOptions *runtimeOptions = NULL;
    // context->coordinatorService can be NULL in test mode
    if (coordService) {
        runtimeOptions = coordService->getRuntimeOptionsFromCoordinator();
        runtimeOptions->checkAndCrashCoordinator("drop_1");
    }

    EntryId entryId = tm.context->logCabinHelper->appendProtoBuf(
            *tm.context->expectedEntryId, state);
    LOG(DEBUG, "LogCabin: DropTable entryId: %lu", entryId);

    if (runtimeOptions)
        runtimeOptions->checkAndCrashCoordinator("drop_2");

    return complete(entryId);
}

void
TableManager::DropTable::complete(EntryId entryId)
{
    Tables::iterator it = tm.tables.find(name);
    if (it == tm.tables.end())
        return;
    uint64_t tableId = it->second;
    tm.tables.erase(it);

    vector<Tablet> removed = tm.removeTabletsForTable(lock, tableId);
    // If a master is down and never receives the dropTabletOwnership
    // call, we don't care, since this tablet has been removed from
    // coordinator's tablet mapping, and hence will not be recovered
    // if / when the master recovers.
    foreach (const auto& tablet, removed) {
            MasterClient::dropTabletOwnership(tm.context,
                                              tablet.serverId,
                                              tableId,
                                              tablet.startKeyHash,
                                              tablet.endKeyHash);
    }

    LOG(NOTICE, "Dropped table '%s' (id %lu), %lu tablets left in map",
        name, tableId, tm.size(lock));

    EntryId tableInfoLogId = tm.getTableInfoLogId(lock, tableId);
    vector<EntryId> invalidates {tableInfoLogId, entryId};

    CoordinatorService *coordService = tm.context->coordinatorService;
    RuntimeOptions *runtimeOptions = NULL;
    // context->coordinatorService can be NULL in test mode
    if (coordService) {
        runtimeOptions = coordService->getRuntimeOptionsFromCoordinator();
        runtimeOptions->checkAndCrashCoordinator("drop_3");
    }

    // If the table being deleted has the largest tableId, then persist
    // the tableId information.
    // This is done so that if the coordinator crashes before the next
    // createTable() and recovers, it should know not to use this tableId
    // again, and start servicing new createTable() rpcs with a higher
    // tableId.
    if (tableId == tm.nextTableId - 1) {
        ProtoBuf::LargestTableId state;
        state.set_entry_type("LargestTableId");
        state.set_table_id(tableId);

        tm.context->logCabinHelper->appendProtoBuf(
                *tm.context->expectedEntryId, state, invalidates);
    } else {
        tm.context->logCabinHelper->invalidate(
                *tm.context->expectedEntryId, invalidates);
    }

    if (runtimeOptions)
        runtimeOptions->checkAndCrashCoordinator("drop_4");
}

void
TableManager::SplitTablet::execute()
{
    ProtoBuf::SplitTablet state;
    state.set_entry_type("SplitTablet");
    state.set_name(name);
    state.set_split_key_hash(splitKeyHash);

    CoordinatorService *coordService = tm.context->coordinatorService;
    RuntimeOptions *runtimeOptions = NULL;
    // context->coordinatorService can be NULL in test mode
    if (coordService) {
        runtimeOptions = coordService->getRuntimeOptionsFromCoordinator();
        runtimeOptions->checkAndCrashCoordinator("split_1");
    }

    EntryId entryId = tm.context->logCabinHelper->appendProtoBuf(
            *tm.context->expectedEntryId, state);
    LOG(DEBUG, "LogCabin: SplitTablet entryId: %lu", entryId);

    if (runtimeOptions)
        runtimeOptions->checkAndCrashCoordinator("split_2");

    complete(entryId);
}

void
TableManager::SplitTablet::complete(EntryId entryId)
{
    Tables::iterator it(tm.tables.find(name));
    if (it == tm.tables.end()) {
        throw NoSuchTable(HERE);
    }
    uint64_t tableId = it->second;

    if (tm.splitExists(lock, tableId, splitKeyHash)) {
        return;
    } else {
        Tablet& originalTablet = tm.getTabletSplit(lock, tableId, splitKeyHash);
        Tablet newTablet = originalTablet;

        originalTablet.endKeyHash = splitKeyHash - 1;
        newTablet.startKeyHash = splitKeyHash;
        tm.map.push_back(newTablet);

        // Tell the master to split the tablet
        MasterClient::splitMasterTablet(tm.context, originalTablet.serverId,
                                        tableId, splitKeyHash);

        // Now append the new table information to LogCabin and invalidate
        // the older table information and split tablet operation information.
        EntryId oldTableInfoEntryId = tm.getTableInfoLogId(lock, tableId);
        vector<EntryId> invalidates {oldTableInfoEntryId, entryId};

        ProtoBuf::TableInformation tableInfo;
        tableInfo.set_entry_type("AliveTable");
        tableInfo.set_name(name);
        tableInfo.set_table_id(tableId);

        // Since we have already split the tablet in local state, we can
        // directly use the local state to populate protobuf reflecting
        // current state of this table to be appended to LogCabin.
        vector<Tablet> allTablets = tm.getTabletsForTable(lock, tableId);
        tableInfo.set_server_span(uint32_t(allTablets.size()));
        foreach (Tablet currentTablet, allTablets) {
            ProtoBuf::TableInformation::TabletInfo&
                        tablet(*tableInfo.add_tablet_info());
            tablet.set_start_key_hash(currentTablet.startKeyHash);
            tablet.set_end_key_hash(currentTablet.endKeyHash);
            tablet.set_master_id(currentTablet.serverId.getId());
            tablet.set_ctime_log_head_id(
                        currentTablet.ctime.getSegmentId());
            tablet.set_ctime_log_head_offset(
                        currentTablet.ctime.getSegmentOffset());
        }

        EntryId newEntryId = tm.context->logCabinHelper->appendProtoBuf(
                *tm.context->expectedEntryId, tableInfo, invalidates);
        LOG(DEBUG, "LogCabin: AliveTable entryId: %lu", newEntryId);
    }
}

void
TableManager::TabletRecovered::execute()
{
    ProtoBuf::TabletRecovered state;
    state.set_entry_type("TabletRecovered");
    state.set_table_id(tableId);
    state.set_start_key_hash(startKeyHash);
    state.set_end_key_hash(endKeyHash);
    state.set_server_id(serverId.getId());
    state.set_ctime_log_head_id(ctime.getSegmentId());
    state.set_ctime_log_head_offset(ctime.getSegmentOffset());

    EntryId entryId = tm.context->logCabinHelper->appendProtoBuf(
            *tm.context->expectedEntryId, state);
    LOG(DEBUG, "LogCabin: TabletRecovered entryId: %lu", entryId);

    complete(entryId);
}

void
TableManager::TabletRecovered::complete(EntryId entryId)
{
    string name;
    for (Tables::iterator it = tm.tables.begin(); it != tm.tables.end(); it++) {
        if (it->second == tableId) {
            name = it->first;
            break;
        }
    }
    // If name for this table doesn't exist, that means we're recovering a table
    // that doesn't actually exist! Something is wrong.
    if (name.empty()) {
        throw NoSuchTable(HERE);
    }

    tm.modifyTablet(lock, tableId, startKeyHash, endKeyHash,
                    serverId, Tablet::NORMAL, ctime);

    // Now append the new table information to LogCabin and invalidate
    // the older table information and tablet recovered operation information.
    EntryId oldTableInfoEntryId = tm.getTableInfoLogId(lock, tableId);
    vector<EntryId> invalidates {oldTableInfoEntryId, entryId};

    ProtoBuf::TableInformation tableInfo;
    tableInfo.set_entry_type("AliveTable");
    tableInfo.set_name(name);
    tableInfo.set_table_id(tableId);

    // Since we have already changed the local state, we can
    // directly use it to populate protobuf reflecting
    // current state of this table to be appended to LogCabin.
    vector<Tablet> allTablets = tm.getTabletsForTable(lock, tableId);
    tableInfo.set_server_span(uint32_t(allTablets.size()));
    foreach (Tablet currentTablet, allTablets) {
        ProtoBuf::TableInformation::TabletInfo&
                    tablet(*tableInfo.add_tablet_info());
        tablet.set_start_key_hash(currentTablet.startKeyHash);
        tablet.set_end_key_hash(currentTablet.endKeyHash);
        tablet.set_master_id(currentTablet.serverId.getId());
        tablet.set_ctime_log_head_id(
                    currentTablet.ctime.getSegmentId());
        tablet.set_ctime_log_head_offset(
                    currentTablet.ctime.getSegmentOffset());
    }

    EntryId newEntryId = tm.context->logCabinHelper->appendProtoBuf(
            *tm.context->expectedEntryId, tableInfo, invalidates);
    LOG(DEBUG, "LogCabin: AliveTable entryId: %lu", newEntryId);
}

/**
 * Add a Tablet to the tablet map.
 *
 *  \param lock
 *      Explicity needs caller to hold a lock.
 * \param tablet
 *      Tablet entry which is copied into the tablet map.
 */
void
TableManager::addTablet(const Lock& lock, const Tablet& tablet)
{
    map.push_back(tablet);
}

/**
 * Get the details of a Tablet in the tablet map by reference. For internal
 * use only since returning a reference to an internal value is not
 * thread-safe.
 *
 *  \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the tablet to return the details of.
 * \param startKeyHash
 *      First key hash that is part of the range of key hashes for the tablet
 *      to return the details of.
 * \param endKeyHash
 *      Last key hash that is part of the range of key hashes for the tablet
 *      to return the details of.
 * \return
 *      A copy of the Tablet entry in the tablet map corresponding to
 *      \a tableId, \a startKeyHash, \a endKeyHash.
 * \throw NoSuchTablet
 *      If the arguments do not identify a tablet currently in the tablet map.
 */
Tablet&
TableManager::find(const Lock& lock,
                   uint64_t tableId,
                   uint64_t startKeyHash,
                   uint64_t endKeyHash)
{
    foreach (auto& tablet, map) {
        if (tablet.tableId == tableId &&
            tablet.startKeyHash == startKeyHash &&
            tablet.endKeyHash == endKeyHash)
            return tablet;
    }
    throw NoSuchTablet(HERE);
}

/// Const version of find().
const Tablet&
TableManager::cfind(const Lock& lock,
                    uint64_t tableId,
                    uint64_t startKeyHash,
                    uint64_t endKeyHash) const
{
    return const_cast<TableManager*>(this)->find(lock, tableId,
                                                 startKeyHash, endKeyHash);
}

/**
 * Check if a tablet has already been split at the given hash value.
 *
 * \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the table containing the tablet to be split if it is not
 *      already split.
 * \param splitKeyHash
 *      Hash value at which the function checks whether there already exists
 *      a split.
 * \return
 *      true if the split already exists and false otherwise
 */

bool
TableManager::splitExists(const Lock& lock, uint64_t tableId,
                           uint64_t splitKeyHash)
{
    int count = 0;
    // check if splitKeyHash falls on the appropriate boundaries of two
    // disjoint hash ranges
    foreach (auto& tablet, map) {
        if (tablet.tableId == tableId &&
            tablet.startKeyHash == splitKeyHash)
            count++;
        else if (tablet.tableId == tableId &&
            tablet.endKeyHash == splitKeyHash - 1)
            count++;
    }
    if (count == 2) {
        return true;
    } else {
        return false;
    }
}

/**
 * Get the details of a Tablet in the tablet map by reference. For internal
 * use only since returning a reference to an internal value is not
 * thread-safe.
 *
 *  \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the tablet to return the details of.
 * \param splitKeyHash
 *      Split key hash that is part of the range of key hashes for the tablet
 *      to return the details of.
 * \return
 *      A copy of the Tablet entry in the tablet map that contains
 *      \a splitKeyHash.
 */

Tablet&
TableManager::getTabletSplit(const Lock& lock,
                               uint64_t tableId,
                               uint64_t splitKeyHash)
{
    // the inner loop is guaranteed to break because splitKeyHash
    // cannot fall outside the entire hash range
    foreach (auto& tablet, map) {
        if (tablet.tableId == tableId &&
            tablet.startKeyHash < splitKeyHash - 1 &&
            splitKeyHash < tablet.endKeyHash) {
            return tablet;
        }
    }
    DIE("could not fnid a containing tablet for splitKeyHash.Something"
        "is terribly wrong in the tablet map");
}

/**
 * Get the LogCabin EntryId corresponding to the information about this table.
 *
 * \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the table for which we want the LogCabin EntryId.
 *
 * \return
 *      LogCabin EntryId corresponding to the information about this table.
 * \throw NoSuchTable
 *      If the arguments do not identify a table currently in the tablesInfo.
 */
EntryId
TableManager::getTableInfoLogId(const Lock& lock, uint64_t tableId)
{
    TablesLogIds::iterator it(tablesLogIds.find(tableId));
    if (it == tablesLogIds.end()) {
        throw NoSuchTable(HERE);
    }
    return (it->second).tableInfoLogId;
}

/**
 * Get the details of a Tablet in the tablet map.
 *
 * \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the tablet to return the details of.
 * \param startKeyHash
 *      First key hash that is part of the range of key hashes for the tablet
 *      to return the details of.
 * \param endKeyHash
 *      Last key hash that is part of the range of key hashes for the tablet
 *      to return the details of.
 * \return
 *      A copy of the Tablet entry in the tablet map corresponding to
 *      \a tableId, \a startKeyHash, \a endKeyHash.
 * \throw NoSuchTablet
 *      If the arguments do not identify a tablet currently in the tablet map.
 */
Tablet
TableManager::getTablet(const Lock& lock,
                        uint64_t tableId,
                        uint64_t startKeyHash,
                        uint64_t endKeyHash) const
{
    return cfind(lock, tableId, startKeyHash, endKeyHash);
}

/**
 * Get the details of all the Tablets in the tablet map that are part of a
 * specific table.
 *
 * \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the table whose tablets are to be returned.
 * \return
 *      List of copies of all the Tablets in the tablet map which are part
 *      of the table indicated by \a tableId.
 */
vector<Tablet>
TableManager::getTabletsForTable(const Lock& lock, uint64_t tableId) const
{
    vector<Tablet> results;
    foreach (const auto& tablet, map) {
        if (tablet.tableId == tableId)
            results.push_back(tablet);
    }
    return results;
}

/**
 * Change the server id, status, or ctime of a Tablet in the tablet map.
 *
 * \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the tablet to return the details of.
 * \param startKeyHash
 *      First key hash that is part of the range of key hashes for the tablet
 *      to return the details of.
 * \param endKeyHash
 *      Last key hash that is part of the range of key hashes for the tablet
 *      to return the details of.
 * \param serverId
 *      Tablet is updated to indicate that it is owned by \a serverId.
 * \param status
 *      Tablet is updated with this status (NORMAL or RECOVERING).
 * \param ctime
 *      Tablet is updated with this Log::Position indicating any object earlier
 *      than \a ctime in its log cannot contain objects belonging to it.
 * \throw NoSuchTablet
 *      If the arguments do not identify a tablet currently in the tablet map.
 */
void
TableManager::modifyTablet(const Lock& lock,
                           uint64_t tableId,
                           uint64_t startKeyHash,
                           uint64_t endKeyHash,
                           ServerId serverId,
                           Tablet::Status status,
                           Log::Position ctime)
{
    Tablet& tablet = find(lock, tableId, startKeyHash, endKeyHash);
    tablet.serverId = serverId;
    tablet.status = status;
    tablet.ctime = ctime;
}

/**
 * Remove all the Tablets in the tablet map that are part of a specific table.
 * Copies of the details about the removed Tablets are returned.
 *
 * \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the table whose tablets are removed.
 * \return
 *      List of copies of all the Tablets in the tablet map which were part
 *      of the table indicated by \a tableId (which have now been removed from
 *      the tablet map).
 */
vector<Tablet>
TableManager::removeTabletsForTable(const Lock& lock, uint64_t tableId)
{
    vector<Tablet> removed;
    auto it = map.begin();
    while (it != map.end()) {
        if (it->tableId == tableId) {
            removed.push_back(*it);
            std::swap(*it, map.back());
            map.pop_back();
        } else {
            ++it;
        }
    }
    return removed;
}

/**
 * Add the LogCabin EntryId corresponding to the information about this table.
 *
 * \param lock
 *      Explicity needs caller to hold a lock.
 * \param tableId
 *      Table id of the table for which we are storing the LogCabin EntryId.
 *  \param entryId
 *      LogCabin EntryId corresponding to the information about this table.
 */
void
TableManager::setTableInfoLogId(const Lock& lock,
                                uint64_t tableId,
                                EntryId entryId)
{
    tablesLogIds[tableId].tableInfoLogId = entryId;
}

/// Return the number of Tablets in the tablet map.
size_t
TableManager::size(const Lock& lock) const
{
    return map.size();
}

} // namespace RAMCloud
