/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Timestamp;

namespace repl {

class HeartbeatResponseAction;
class MemberData;
class OpTime;
class ReplSetHeartbeatArgs;
class ReplSetConfig;
class TagSubgroup;
class LastVote;
struct MemberState;

/**
 * Replication Topology Coordinator interface.
 *
 * This object is responsible for managing the topology of the cluster.
 * Tasks include consensus and leader election, chaining, and configuration management.
 * Methods of this class should be non-blocking.
 */
class TopologyCoordinator {
    MONGO_DISALLOW_COPYING(TopologyCoordinator);

public:
    class Role;

    virtual ~TopologyCoordinator();

    /**
     * Different modes a node can be in while still reporting itself as in state PRIMARY.
     *
     * Valid transitions:
     *
     *       kNotLeader <----------------------------------
     *          |                                         |
     *          |                                         |
     *          |                                         |
     *          v                                         |
     *       kLeaderElect-----                            |
     *          |            |                            |
     *          |            |                            |
     *          v            |                            |
     *       kMaster -------------------------            |
     *        |  ^           |                |           |
     *        |  |     -------------------    |           |
     *        |  |     |                 |    |           |
     *        v  |     v                 v    v           |
     *  kAttemptingStepDown----------->kSteppingDown      |
     *        |                              |            |
     *        |                              |            |
     *        |                              |            |
     *        ---------------------------------------------
     *
     */
    enum class LeaderMode {
        kNotLeader,           // This node is not currently a leader.
        kLeaderElect,         // This node has been elected leader, but can't yet accept writes.
        kMaster,              // This node reports ismaster:true and can accept writes.
        kSteppingDown,        // This node is in the middle of a (hb) stepdown that must complete.
        kAttemptingStepDown,  // This node is in the middle of a stepdown (cmd) that might fail.
    };

    ////////////////////////////////////////////////////////////
    //
    // State inspection methods.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Gets the role of this member in the replication protocol.
     */
    virtual Role getRole() const = 0;

    /**
     * Gets the MemberState of this member in the replica set.
     */
    virtual MemberState getMemberState() const = 0;

    /**
     * Returns whether this node should be allowed to accept writes.
     */
    virtual bool canAcceptWrites() const = 0;

    /**
     * Returns true if this node is in the process of stepping down.  Note that this can be
     * due to an unconditional stepdown that must succeed (for instance from learning about a new
     * term) or due to a stepdown attempt that could fail (for instance from a stepdown cmd that
     * could fail if not enough nodes are caught up).
     */
    virtual bool isSteppingDown() const = 0;

    /**
     * Returns the address of the current sync source, or an empty HostAndPort if there is no
     * current sync source.
     */
    virtual HostAndPort getSyncSourceAddress() const = 0;

    /**
     * Retrieves a vector of HostAndPorts containing all nodes that are neither DOWN nor
     * ourself.
     */
    virtual std::vector<HostAndPort> getMaybeUpHostAndPorts() const = 0;

    /**
     * Gets the earliest time the current node will stand for election.
     */
    virtual Date_t getStepDownTime() const = 0;

    /**
     * Gets the current value of the maintenance mode counter.
     */
    virtual int getMaintenanceCount() const = 0;

    /**
     * Gets the latest term this member is aware of. If this member is the primary,
     * it's the current term of the replica set.
     */
    virtual long long getTerm() = 0;

    enum class UpdateTermResult { kAlreadyUpToDate, kTriggerStepDown, kUpdatedTerm };

    ////////////////////////////////////////////////////////////
    //
    // Basic state manipulation methods.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Sets the latest term this member is aware of to the higher of its current value and
     * the value passed in as "term".
     * Returns the result of setting the term value, or if a stepdown should be triggered.
     */
    virtual UpdateTermResult updateTerm(long long term, Date_t now) = 0;

    /**
     * Sets the index into the config used when we next choose a sync source
     */
    virtual void setForceSyncSourceIndex(int index) = 0;

    enum class ChainingPreference { kAllowChaining, kUseConfiguration };

    /**
     * Chooses and sets a new sync source, based on our current knowledge of the world.
     */
    virtual HostAndPort chooseNewSyncSource(Date_t now,
                                            const OpTime& lastOpTimeFetched,
                                            ChainingPreference chainingPreference) = 0;

    /**
     * Suppresses selecting "host" as sync source until "until".
     */
    virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) = 0;

    /**
     * Removes a single entry "host" from the list of potential sync sources which we
     * have blacklisted, if it is supposed to be unblacklisted by "now".
     */
    virtual void unblacklistSyncSource(const HostAndPort& host, Date_t now) = 0;

    /**
     * Clears the list of potential sync sources we have blacklisted.
     */
    virtual void clearSyncSourceBlacklist() = 0;

    /**
     * Determines if a new sync source should be chosen, if a better candidate sync source is
     * available.  If the current sync source's last optime ("syncSourceLastOpTime" under
     * protocolVersion 1, but pulled from the MemberData in protocolVersion 0) is more than
     * _maxSyncSourceLagSecs behind any syncable source, this function returns true. If we are
     * running in ProtocolVersion 1, our current sync source is not primary, has no sync source
     * ("syncSourceHasSyncSource" is false), and only has data up to "myLastOpTime", returns true.
     *
     * "now" is used to skip over currently blacklisted sync sources.
     *
     * TODO (SERVER-27668): Make OplogQueryMetadata non-optional in mongodb 3.8.
     */
    virtual bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                        const rpc::ReplSetMetadata& replMetadata,
                                        boost::optional<rpc::OplogQueryMetadata> oqMetadata,
                                        Date_t now) const = 0;

    /**
     * Checks whether we are a single node set and we are not in a stepdown period.  If so,
     * puts us into candidate mode, otherwise does nothing.  This is used to ensure that
     * nodes in a single node replset become primary again when their stepdown period ends.
     */
    virtual bool becomeCandidateIfStepdownPeriodOverAndSingleNodeSet(Date_t now) = 0;

    /**
     * Sets the earliest time the current node will stand for election to "newTime".
     *
     * Until this time, while the node may report itself as electable, it will not stand
     * for election.
     */
    virtual void setElectionSleepUntil(Date_t newTime) = 0;

    /**
     * Sets the reported mode of this node to one of RS_SECONDARY, RS_STARTUP2, RS_ROLLBACK or
     * RS_RECOVERING, when getRole() == Role::follower.  This is the interface by which the
     * applier changes the reported member state of the current node, and enables or suppresses
     * electability of the current node.  All modes but RS_SECONDARY indicate an unelectable
     * follower state (one that cannot transition to candidate).
     */
    virtual void setFollowerMode(MemberState::MS newMode) = 0;

    /**
     * Scan the memberData and determine the highest last applied or last
     * durable optime present on a majority of servers; set _lastCommittedOpTime to this
     * new entry.
     * Whether the last applied or last durable op time is used depends on whether
     * the config getWriteConcernMajorityShouldJournal is set.
     * Returns true if the _lastCommittedOpTime was changed.
     */
    virtual bool updateLastCommittedOpTime() = 0;

    /**
     * Updates _lastCommittedOpTime to be "committedOpTime" if it is more recent than the
     * current last committed OpTime.  Returns true if _lastCommittedOpTime is changed.
     */
    virtual bool advanceLastCommittedOpTime(const OpTime& committedOpTime) = 0;

    /**
     * Returns the OpTime of the latest majority-committed op known to this server.
     */
    virtual OpTime getLastCommittedOpTime() const = 0;

    /**
     * Called by the ReplicationCoordinator to signal that we have finished catchup and drain modes
     * and are ready to fully become primary and start accepting writes.
     * "firstOpTimeOfTerm" is a floor on the OpTimes this node will be allowed to consider committed
     * for this tenure as primary. This prevents entries from before our election from counting as
     * committed in our view, until our election (the "firstOpTimeOfTerm" op) has been committed.
     */
    virtual void completeTransitionToPrimary(const OpTime& firstOpTimeOfTerm) = 0;

    /**
     * Adjusts the maintenance mode count by "inc".
     *
     * It is an error to call this method if getRole() does not return Role::follower.
     * It is an error to allow the maintenance count to go negative.
     */
    virtual void adjustMaintenanceCountBy(int inc) = 0;

    ////////////////////////////////////////////////////////////
    //
    // Methods that prepare responses to command requests.
    //
    ////////////////////////////////////////////////////////////

    // produces a reply to a replSetSyncFrom command
    virtual void prepareSyncFromResponse(const HostAndPort& target,
                                         BSONObjBuilder* response,
                                         Status* result) = 0;

    // produce a reply to a replSetFresh command
    virtual void prepareFreshResponse(const ReplicationCoordinator::ReplSetFreshArgs& args,
                                      Date_t now,
                                      BSONObjBuilder* response,
                                      Status* result) = 0;

    // produce a reply to a received electCmd
    virtual void prepareElectResponse(const ReplicationCoordinator::ReplSetElectArgs& args,
                                      Date_t now,
                                      BSONObjBuilder* response,
                                      Status* result) = 0;

    // produce a reply to a heartbeat
    virtual Status prepareHeartbeatResponse(Date_t now,
                                            const ReplSetHeartbeatArgs& args,
                                            const std::string& ourSetName,
                                            ReplSetHeartbeatResponse* response) = 0;

    // produce a reply to a V1 heartbeat
    virtual Status prepareHeartbeatResponseV1(Date_t now,
                                              const ReplSetHeartbeatArgsV1& args,
                                              const std::string& ourSetName,
                                              ReplSetHeartbeatResponse* response) = 0;

    struct ReplSetStatusArgs {
        Date_t now;
        unsigned selfUptime;
        const OpTime& readConcernMajorityOpTime;
        const BSONObj& initialSyncStatus;
    };

    // produce a reply to a status request
    virtual void prepareStatusResponse(const ReplSetStatusArgs& rsStatusArgs,
                                       BSONObjBuilder* response,
                                       Status* result) = 0;

    // Produce a replSetUpdatePosition command to be sent to the node's sync source.
    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand(
        ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle,
        OpTime currentCommittedSnapshotOpTime) const = 0;

    // produce a reply to an ismaster request.  It is only valid to call this if we are a
    // replset.
    virtual void fillIsMasterForReplSet(IsMasterResponse* response) = 0;

    // Produce member data for the serverStatus command and diagnostic logging.
    virtual void fillMemberData(BSONObjBuilder* result) = 0;

    enum class PrepareFreezeResponseResult { kNoAction, kElectSelf };

    /**
     * Produce a reply to a freeze request. Returns a PostMemberStateUpdateAction on success that
     * may trigger state changes in the caller.
     */
    virtual StatusWith<PrepareFreezeResponseResult> prepareFreezeResponse(
        Date_t now, int secs, BSONObjBuilder* response) = 0;

    ////////////////////////////////////////////////////////////
    //
    // Methods for sending and receiving heartbeats,
    // reconfiguring and handling the results of standing for
    // election.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Updates the topology coordinator's notion of the replica set configuration.
     *
     * "newConfig" is the new configuration, and "selfIndex" is the index of this
     * node's configuration information in "newConfig", or "selfIndex" is -1 to
     * indicate that this node is not a member of "newConfig".
     *
     * newConfig.isInitialized() should be true, though implementations may accept
     * configurations where this is not true, for testing purposes.
     */
    virtual void updateConfig(const ReplSetConfig& newConfig, int selfIndex, Date_t now) = 0;

    /**
     * Prepares a heartbeat request appropriate for sending to "target", assuming the
     * current time is "now".  "ourSetName" is used as the name for our replica set if
     * the topology coordinator does not have a valid configuration installed.
     *
     * The returned pair contains proper arguments for a replSetHeartbeat command, and
     * an amount of time to wait for the response.
     *
     * This call should be paired (with intervening network communication) with a call to
     * processHeartbeatResponse for the same "target".
     */
    virtual std::pair<ReplSetHeartbeatArgs, Milliseconds> prepareHeartbeatRequest(
        Date_t now, const std::string& ourSetName, const HostAndPort& target) = 0;
    virtual std::pair<ReplSetHeartbeatArgsV1, Milliseconds> prepareHeartbeatRequestV1(
        Date_t now, const std::string& ourSetName, const HostAndPort& target) = 0;

    /**
     * Processes a heartbeat response from "target" that arrived around "now", having
     * spent "networkRoundTripTime" millis on the network.
     *
     * Updates internal topology coordinator state, and returns instructions about what action
     * to take next.
     *
     * If the next action indicates StartElection, the topology coordinator has transitioned to
     * the "candidate" role, and will remain there until processWinElection or
     * processLoseElection are called.
     *
     * If the next action indicates "StepDownSelf", the topology coordinator has transitioned
     * to the "follower" role from "leader", and the caller should take any necessary actions
     * to become a follower.
     *
     * If the next action indicates "StepDownRemotePrimary", the caller should take steps to
     * cause the specified remote host to step down from primary to secondary.
     *
     * If the next action indicates "Reconfig", the caller should verify the configuration in
     * hbResponse is acceptable, perform any other reconfiguration actions it must, and call
     * updateConfig with the new configuration and the appropriate value for "selfIndex".  It
     * must also wrap up any outstanding elections (by calling processLoseElection or
     * processWinElection) before calling updateConfig.
     *
     * This call should be paired (with intervening network communication) with a call to
     * prepareHeartbeatRequest for the same "target".
     */
    virtual HeartbeatResponseAction processHeartbeatResponse(
        Date_t now,
        Milliseconds networkRoundTripTime,
        const HostAndPort& target,
        const StatusWith<ReplSetHeartbeatResponse>& hbResponse) = 0;

    /**
     *  Returns whether or not at least 'numNodes' have reached the given opTime.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    virtual bool haveNumNodesReachedOpTime(const OpTime& opTime,
                                           int numNodes,
                                           bool durablyWritten) = 0;

    /**
     * Returns whether or not at least one node matching the tagPattern has reached
     * the given opTime.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    virtual bool haveTaggedNodesReachedOpTime(const OpTime& opTime,
                                              const ReplSetTagPattern& tagPattern,
                                              bool durablyWritten) = 0;

    /**
     * Returns a vector of members that have applied the operation with OpTime 'op'.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     * "skipSelf" means to exclude this node whether or not the op has been applied.
     */
    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op,
                                                       bool durablyWritten,
                                                       bool skipSelf) = 0;

    /**
     * Marks a member as down from our perspective and returns a bool which indicates if we can no
     * longer see a majority of the nodes and thus should step down.
     */
    virtual bool setMemberAsDown(Date_t now, const int memberIndex) = 0;

    /**
     * Goes through the memberData and determines which member that is currently live
     * has the stalest (earliest) last update time.  Returns (-1, Date_t::max()) if there are
     * no other members.
     */
    virtual std::pair<int, Date_t> getStalestLiveMember() const = 0;

    /**
     * Go through the memberData, and mark nodes which haven't been updated
     * recently (within an election timeout) as "down".  Returns a HeartbeatResponseAction, which
     * will be StepDownSelf if we can no longer see a majority of the nodes, otherwise NoAction.
     */
    virtual HeartbeatResponseAction checkMemberTimeouts(Date_t now) = 0;

    /**
     * Set all nodes in memberData to not stale with a lastUpdate of "now".
     */
    virtual void resetAllMemberTimeouts(Date_t now) = 0;

    /**
     * Set all nodes in memberData that are present in member_set
     * to not stale with a lastUpdate of "now".
     */
    virtual void resetMemberTimeouts(Date_t now,
                                     const stdx::unordered_set<HostAndPort>& member_set) = 0;

    /*
     * Returns the last optime that this node has applied, whether or not it has been journaled.
     */
    virtual OpTime getMyLastAppliedOpTime() const = 0;

    /*
     * Returns the last optime that this node has applied and journaled.
     */
    virtual OpTime getMyLastDurableOpTime() const = 0;

    /*
     * Returns information we have on the state of this node.
     */
    virtual MemberData* getMyMemberData() = 0;

    /*
     * Returns information we have on the state of the node identified by memberId.  Returns
     * nullptr if memberId is not found in the configuration.
     */
    virtual MemberData* findMemberDataByMemberId(const int memberId) = 0;

    /*
     * Returns information we have on the state of the node identified by rid.  Returns
     * nullptr if rid is not found in the heartbeat data.  This method is used only for
     * master/slave replication.
     */
    virtual MemberData* findMemberDataByRid(const OID rid) = 0;

    /*
     * Adds and returns a memberData entry for the given RID.
     * Used only in master/slave mode.
     */
    virtual MemberData* addSlaveMemberData(const OID rid) = 0;

    /**
     * If getRole() == Role::candidate and this node has not voted too recently, updates the
     * lastVote tracker and returns true.  Otherwise, returns false.
     */
    virtual bool voteForMyself(Date_t now) = 0;

    /**
     * Sets lastVote to be for ourself in this term.
     */
    virtual void voteForMyselfV1() = 0;

    /**
     * Sets election id and election optime.
     */
    virtual void setElectionInfo(OID electionId, Timestamp electionOpTime) = 0;

    /**
     * Performs state updates associated with winning an election.
     *
     * It is an error to call this if the topology coordinator is not in candidate mode.
     *
     * Exactly one of either processWinElection or processLoseElection must be called if
     * processHeartbeatResponse returns StartElection, to exit candidate mode.
     */
    virtual void processWinElection(OID electionId, Timestamp electionOpTime) = 0;

    /**
     * Performs state updates associated with losing an election.
     *
     * It is an error to call this if the topology coordinator is not in candidate mode.
     *
     * Exactly one of either processWinElection or processLoseElection must be called if
     * processHeartbeatResponse returns StartElection, to exit candidate mode.
     */
    virtual void processLoseElection() = 0;

    /**
     * Readies the TopologyCoordinator for an attempt to stepdown that may fail.  This is used
     * when we receive a stepdown command (which can fail if not enough secondaries are caught up)
     * to ensure that we never process more than one stepdown request at a time.
     * Returns OK if it is safe to continue with the stepdown attempt, or returns
     * ConflictingOperationInProgess if this node is already processing a stepdown request of any
     * kind.
     */
    virtual Status prepareForStepDownAttempt() = 0;

    /**
     * If this node is still attempting to process a stepdown attempt, aborts the attempt and
     * returns this node to normal primary/master state.  If this node has already completed
     * stepping down or is now in the process of handling an unconditional stepdown, then this
     * method does nothing.
     */
    virtual void abortAttemptedStepDownIfNeeded() = 0;

    /**
     * Tries to transition the coordinator from the leader role to the follower role.
     *
     * A step down succeeds based on the following conditions:
     *
     *      C1. 'force' is true and now > waitUntil
     *
     *      C2. A majority set of nodes, M, in the replica set have optimes greater than or
     *      equal to the last applied optime of the primary.
     *
     *      C3. There exists at least one electable secondary node in the majority set M.
     *
     *
     * If C1 is true, or if both C2 and C3 are true, then the stepdown occurs and this method
     * returns true. If the conditions for successful stepdown aren't met yet, but waiting for more
     * time to pass could make it succeed, returns false.  If the whole stepdown attempt should be
     * abandoned (for example because the time limit expired or because we've already stepped down),
     * throws an exception.
     * TODO(spencer): Unify with the finishUnconditionalStepDown() method.
     */
    virtual bool attemptStepDown(
        long long termAtStart, Date_t now, Date_t waitUntil, Date_t stepDownUntil, bool force) = 0;

    /**
     * Returns whether it is safe for a stepdown attempt to complete, ignoring the 'force' argument.
     * This is essentially checking conditions C2 and C3 as described in the comment to
     * attemptStepDown().
     */
    virtual bool isSafeToStepDown() = 0;

    /**
     * Readies the TopologyCoordinator for stepdown.  Returns false if we're already in the process
     * of an unconditional step down.  If we are in the middle of a stepdown command attempt when
     * this is called then this unconditional stepdown will supersede the stepdown attempt, which
     * will cause the stepdown to fail.  When this returns true it must be followed by a call to
     * finishUnconditionalStepDown() that is called when holding the global X lock.
     */
    virtual bool prepareForUnconditionalStepDown() = 0;

    /**
     * Sometimes a request to step down comes in (like via a heartbeat), but we don't have the
     * global exclusive lock so we can't actually stepdown at that moment. When that happens
     * we record that a stepdown request is pending (by calling prepareForUnconditionalStepDown())
     * and schedule work to stepdown in the global X lock.  This method is called after holding the
     * global lock to perform the actual stepdown.
     * TODO(spencer): Unify with the finishAttemptedStepDown() method.
     */
    virtual void finishUnconditionalStepDown() = 0;

    /**
     * Considers whether or not this node should stand for election, and returns true
     * if the node has transitioned to candidate role as a result of the call.
     */
    virtual Status checkShouldStandForElection(Date_t now) const = 0;

    /**
     * Set the outgoing heartbeat message from self
     */
    virtual void setMyHeartbeatMessage(const Date_t now, const std::string& s) = 0;

    /**
     * Prepares a ReplSetMetadata object describing the current term, primary, and lastOp
     * information.
     */
    virtual rpc::ReplSetMetadata prepareReplSetMetadata(const OpTime& lastVisibleOpTime) const = 0;

    /**
     * Prepares an OplogQueryMetadata object describing the current sync source, rbid, primary,
     * lastOpApplied, and lastOpCommitted.
     */
    virtual rpc::OplogQueryMetadata prepareOplogQueryMetadata(int rbid) const = 0;

    /**
     * Writes into 'output' all the information needed to generate a summary of the current
     * replication state for use by the web interface.
     */
    virtual void summarizeAsHtml(ReplSetHtmlSummary* output) = 0;

    /**
     * Prepares a ReplSetRequestVotesResponse.
     */
    virtual void processReplSetRequestVotes(const ReplSetRequestVotesArgs& args,
                                            ReplSetRequestVotesResponse* response) = 0;

    /**
     * Loads an initial LastVote document, which was read from local storage.
     *
     * Called only during replication startup. All other updates are done internally.
     */
    virtual void loadLastVote(const LastVote& lastVote) = 0;

    /**
     * Updates the current primary index.
     */
    virtual void setPrimaryIndex(long long primaryIndex) = 0;

    /**
     * Returns the current primary index.
     */
    virtual int getCurrentPrimaryIndex() const = 0;

    enum StartElectionReason {
        kElectionTimeout,
        kPriorityTakeover,
        kStepUpRequest,
        kCatchupTakeover
    };

    /**
     * Transitions to the candidate role if the node is electable.
     */
    virtual Status becomeCandidateIfElectable(const Date_t now, StartElectionReason reason) = 0;

    /**
     * Updates the storage engine read committed support in the TopologyCoordinator options after
     * creation.
     */
    virtual void setStorageEngineSupportsReadCommitted(bool supported) = 0;

    /**
     * Reset the booleans to record the last heartbeat restart.
     */
    virtual void restartHeartbeats() = 0;

    /**
     * Scans through all members that are 'up' and return the latest known optime, if we have
     * received (successful or failed) heartbeats from all nodes since heartbeat restart.
     *
     * Returns boost::none if any node hasn't responded to a heartbeat since we last restarted
     * heartbeats.
     * Returns OpTime(Timestamp(0, 0), 0), the smallest OpTime in PV1, if other nodes are all down.
     */
    virtual boost::optional<OpTime> latestKnownOpTimeSinceHeartbeatRestart() const = 0;

protected:
    TopologyCoordinator() {}
};

/**
 * Type that denotes the role of a node in the replication protocol.
 *
 * The role is distinct from MemberState, in that it only deals with the
 * roles a node plays in the basic protocol -- leader, follower and candidate.
 * The mapping between MemberState and Role is complex -- several MemberStates
 * map to the follower role, and MemberState::RS_SECONDARY maps to either
 * follower or candidate roles, e.g.
 */
class TopologyCoordinator::Role {
public:
    /**
     * Constant indicating leader role.
     */
    static const Role leader;

    /**
     * Constant indicating follower role.
     */
    static const Role follower;

    /**
     * Constant indicating candidate role
     */
    static const Role candidate;

    Role() {}

    bool operator==(Role other) const {
        return _value == other._value;
    }
    bool operator!=(Role other) const {
        return _value != other._value;
    }

    std::string toString() const;

private:
    explicit Role(int value);

    int _value;
};

//
// Convenience method for unittest code. Please use accessors otherwise.
//

std::ostream& operator<<(std::ostream& os, TopologyCoordinator::Role role);
std::ostream& operator<<(std::ostream& os, TopologyCoordinator::PrepareFreezeResponseResult result);

}  // namespace repl
}  // namespace mongo
