/*
 * S2E Selective Symbolic Execution Framework
 *
 * Copyright (c) 2010, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Currently maintained by:
 *    Vitaly Chipounov <vitaly.chipounov@epfl.ch>
 *    Volodymyr Kuznetsov <vova.kuznetsov@epfl.ch>
 *
 * All contributors are listed in S2E-AUTHORS file.
 *
 */

#include <llvm/System/TimeValue.h>

#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>
#include <s2e/s2e_qemu.h>

#include "StateManager.h"
#include <klee/Searcher.h>

namespace s2e {
namespace plugins {

S2E_DEFINE_PLUGIN(StateManager, "Control the deletion/suspension of states", "StateManager",
                  "ModuleExecutionDetector");

void sm_callback(S2EExecutionState *s, bool killingState)
{
    StateManager *sm = static_cast<StateManager*>(g_s2e->getPlugin("StateManager"));
    assert(sm);

    if (!sm->grabLock()) {
        return;
    }

    if (killingState && s) {
        sm->resumeSucceededState(s);
        sm->ungrabLock();
        return;
    }

    //If there are no states, try to resume some successful ones
    if (g_s2e->getExecutor()->getStatesCount() == 0) {
        sm->killAllButOneSuccessful();
        sm->ungrabLock();
        return;
    }

    //Check for timeout conditions
    sm->killOnTimeOut();
    sm->ungrabLock();
}

void StateManager::sendKillToAllInstances(bool keepOneSuccessful, unsigned procId)
{
    suspendAllProcesses();

    StateManagerShared *s = m_shared.get();

    StateManagerShared::Command cmd;
    cmd.command = StateManagerShared::KILL;
    cmd.nodeId = keepOneSuccessful ? procId : (unsigned)-1;
    s->command.write(cmd);
}

bool StateManager::listenForCommands()
{
    StateManagerShared *s = m_shared.get();
    AtomicFunctions::add(&s->waitingProcessCount, 1);

    StateManagerShared::Command cmd;
    do {
     cmd = s->command.read();
    }while(cmd.command != StateManagerShared::EMPTY);

    if (cmd.command == StateManagerShared::KILL) {
        if (cmd.nodeId == s2e()->getCurrentProcessId()) {
            //Keep one successful
            killAllButOneSuccessfulLocal();
        }else {
            //Kill everything
            StateSet toKeep;
            killAllExcept(toKeep, true);
        }
    }

    AtomicFunctions::sub(&s->waitingProcessCount, 1);
    return cmd.command == StateManagerShared::RESUME;
}

void StateManager::suspendAllProcesses()
{
    StateManagerShared *s = m_shared.get();
    AtomicFunctions::write(&s->suspendAll, 1);

    //Wait for all instances to be suspended (except our own one)
    while (AtomicFunctions::read(&s->waitingProcessCount) < g_s2e->getMaxProcesses() - 1)
        ;
}

bool StateManager::isSuspending()
{
    StateManagerShared *s = m_shared.get();
    return AtomicFunctions::read(&s->suspendAll) == 1;
}

bool StateManager::grabLock()
{
    StateManagerShared *s;
    while (!(s = m_shared.tryAcquire())) {
        if (isSuspending()) {
            if (listenForCommands()) {
                continue;
            }
            //This is unreachable
            assert(false && "Unreachable");
            return false;
        }
     }
    return true;
}

void StateManager::ungrabLock()
{
    m_shared.release();
}

bool StateManager::timeoutReached() const
{
    if (!m_timeout) {
        return false;
    }

    llvm::sys::TimeValue curTime = llvm::sys::TimeValue::now();
    llvm::sys::TimeValue prevTime((double)AtomicFunctions::read(&m_shared.get()->timeOfLastNewBlock));

    return curTime.seconds() - prevTime.seconds() >= m_timeout;
}

void StateManager::resetTimeout()
{
    llvm::sys::TimeValue curTime = llvm::sys::TimeValue::now();
    AtomicFunctions::write(&m_shared.get()->timeOfLastNewBlock, curTime.seconds());
}

void StateManager::resumeSucceeded()
{
    foreach2(it, m_succeeded.begin(), m_succeeded.end()) {
        m_executor->resumeState(*it);
    }
    m_succeeded.clear();
}

bool StateManager::resumeSucceededState(S2EExecutionState *s)
{
    if (m_succeeded.find(s) != m_succeeded.end()) {
        m_succeeded.erase(s);
        m_executor->resumeState(s);
        return true;
    }
    return false;
}

void StateManager::initialize()
{
    ConfigFile *cfg = s2e()->getConfig();

    m_timeout = cfg->getInt(getConfigKey() + ".timeout");
    resetTimeout();

    m_detector = static_cast<ModuleExecutionDetector*>(s2e()->getPlugin("ModuleExecutionDetector"));

    m_detector->onModuleTranslateBlockStart.connect(
            sigc::mem_fun(*this,
                    &StateManager::onNewBlockCovered)
            );

    s2e()->getCorePlugin()->onProcessFork.connect(
            sigc::mem_fun(*this,
                    &StateManager::onProcessFork)
            );

    s2e()->getCorePlugin()->onCustomInstruction.connect(
            sigc::mem_fun(*this, &StateManager::onCustomInstruction));

    m_executor = s2e()->getExecutor();
    m_executor->setStateManagerCb(sm_callback);
}

void StateManager::onProcessFork()
{
    m_shared.get()->successCount[s2e()->getCurrentProcessId()] = 0;
}

//Reset the timeout every time a new block of the module is translated.
//XXX: this is an approximation. The cache could be flushed in between.
void StateManager::onNewBlockCovered(
        ExecutionSignal *signal,
        S2EExecutionState* state,
        const ModuleDescriptor &module,
        TranslationBlock *tb,
        uint64_t pc)
{
    s2e()->getDebugStream() << "New block " << std::hex << pc << " discovered" << std::endl;
    resetTimeout();
}

void StateManager::killOnTimeOut()
{
    if (!timeoutReached()) {
        return;
    }

    s2e()->getDebugStream() << "No more blocks found in " <<
            std::dec << m_timeout << " seconds, killing states."
            << std::endl;

    //Reset the counter here to avoid being called again
    //(killAllButOneSuccessful will throw an exception if it deletes the current state).
    resetTimeout();

    if (!killAllButOneSuccessful()) {
        s2e()->getDebugStream() << "There are no successful states to kill..."  << std::endl;
    }
}

bool StateManager::killAllExcept(StateSet &toKeep, bool ungrab)
{
    bool killCurrent = false;
    const std::set<klee::ExecutionState*> &states = s2e()->getExecutor()->getStates();
    std::set<klee::ExecutionState*>::const_iterator it = states.begin();

    while(it != states.end()) {
        S2EExecutionState *curState = static_cast<S2EExecutionState*>(*it);
        if (toKeep.find(curState) != toKeep.end()) {
            ++it;
            continue;
        }

        ++it;
        if (curState == g_s2e_state) {
            killCurrent = true;
        }else {
            s2e()->getExecutor()->terminateStateEarly(*curState, "StateManager: killing state");
        }
    }

    //In case we need to kill the current state, do it last, because it will throw and exception
    //and return to the state scheduler.
    if (killCurrent) {
        if (ungrab) {
            ungrabLock();
        }
        s2e()->getExecutor()->terminateStateEarly(*g_s2e_state, "StateManager: killing state");
    }

    return true;
}

void StateManager::killAllButOneSuccessfulLocal()
{
    assert(m_succeeded.size() > 0);
    S2EExecutionState *one =  *m_succeeded.begin();
    resumeSucceeded();

    StateSet toKeep;
    toKeep.insert(one);
    killAllExcept(toKeep, true);
}

bool StateManager::killAllButOneSuccessful()
{
    uint64_t *successCount = m_shared.get()->successCount;
    unsigned maxProcesses = s2e()->getMaxProcesses();

    //Determine the instance that has at least one successful state
    unsigned hasSuccessfulIndex;
    for (unsigned hasSuccessfulIndex=0; hasSuccessfulIndex < maxProcesses; ++hasSuccessfulIndex) {
        if (successCount[hasSuccessfulIndex] > 0) {
            break;
        }
    }

    //There are no successful states anywhere, just return
    if (hasSuccessfulIndex == maxProcesses) {
        return false;
    }

    s2e()->getDebugStream() << "Killing all but one successful on node " << std::dec << hasSuccessfulIndex << std::endl;

    //Kill all states everywhere except one successful on the instance that we found
    if (hasSuccessfulIndex == s2e()->getCurrentProcessId()) {
        //We chose one state on our local instance
        assert(m_succeeded.size() > 0);

        //Ask other instances to kill all their states
        sendKillToAllInstances(false, 0);

        //Kill all local states
        killAllButOneSuccessfulLocal();
    }else {
        //We chose a state on a different instance
        sendKillToAllInstances(true, hasSuccessfulIndex);

        //Kill everything locally
        StateSet toKeep;
        killAllExcept(toKeep, true);
    }

    return true;
}

/*********************************************************************************************/
/*********************************************************************************************/
/*********************************************************************************************/

bool StateManager::succeedState(S2EExecutionState *s)
{
    if (!grabLock()) {
        return false;
    }

    s2e()->getDebugStream() << "Succeeding state " << std::dec << s->getID() << std::endl;

    if (m_succeeded.find(s) != m_succeeded.end()) {
        //Do not suspend states that were consecutively succeeded.
        s2e()->getDebugStream() << "State " << std::dec << s->getID() <<
                " was already marked as succeeded" << std::endl;
        ungrabLock();
        return false;
    }
    m_succeeded.insert(s);

    bool ret =  s2e()->getExecutor()->suspendState(s);
    m_shared.get()->successCount[s2e()->getCurrentProcessId()] = m_succeeded.size();

    ungrabLock();
    return ret;
}

bool StateManager::empty()
{
    assert(s2e()->getExecutor()->getSearcher());
    return s2e()->getExecutor()->getSearcher()->empty();
}

//Allows to control behavior directly from guest code
void StateManager::onCustomInstruction(S2EExecutionState* state, uint64_t opcode)
{
    if (!OPCODE_CHECK(opcode, STATE_MANAGER_OPCODE)) {
        return;
    }

    unsigned subfunc = OPCODE_GETSUBFUNCTION(opcode);
    switch (subfunc) {
        case SUCCEED:
            succeedState(state);
            break;

        default:
            s2e()->getWarningsStream() << "StateManager: incorrect opcode " << std::hex << subfunc << std::endl;
    }
}

}
}
