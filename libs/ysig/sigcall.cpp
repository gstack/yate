/**
 * sigcall.cpp
 * This file is part of the YATE Project http://YATE.null.ro 
 *
 * Yet Another Signalling Stack - implements the support for SS7, ISDN and PSTN
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "yatesig.h"

#include <stdlib.h>


using namespace TelEngine;

/**
 * SignallingCallControl
 */
SignallingCallControl::SignallingCallControl(const NamedList& params)
    : Mutex(true),
    m_circuits(0),
    m_strategy(SignallingCircuitGroup::Increment),
    m_exiting(false),
    m_dumper(0)
{
    // Strategy
    const char* strategy = params.getValue("strategy","increment");
    m_strategy = SignallingCircuitGroup::str2strategy(strategy);
    String restrict;
    if (m_strategy != SignallingCircuitGroup::Random)
	restrict = params.getValue("strategy-restrict");
    if (!restrict.null())
	if (restrict == "odd")
	    m_strategy |= SignallingCircuitGroup::OnlyOdd;
	else if (restrict == "even")
	    m_strategy |= SignallingCircuitGroup::OnlyEven;
	else if (restrict == "odd-fallback")
	    m_strategy |= SignallingCircuitGroup::OnlyOdd | SignallingCircuitGroup::Fallback;
	else if (restrict == "even-fallback")
	    m_strategy |= SignallingCircuitGroup::OnlyEven | SignallingCircuitGroup::Fallback;
}

SignallingCallControl::~SignallingCallControl()
{
    attach((SignallingCircuitGroup*)0);
}

unsigned int SignallingCallControl::circuitCount()
{
    Lock lock(this);
    return m_circuits ? m_circuits->count() : 0;
}

// Attach a signalling circuit group. Set its strategy
void SignallingCallControl::attach(SignallingCircuitGroup* circuits)
{
    Lock lock(this);
    // Don't attach if it's the same object
    if (m_circuits == circuits)
	return;
    cleanup(circuits ? "circuit group attach" : "circuit group detach");
    if (m_circuits && circuits)
	Debug(DebugNote,
	    "SignallingCallControl. Replaced circuit group (%p) with (%p) [%p]",
	    m_circuits,circuits,this);
    m_circuits = circuits;
    if (m_circuits)
	m_circuits->setStrategy(m_strategy);
}

// Reserve a circuit from a given list in attached group
bool SignallingCallControl::reserveCircuit(SignallingCircuit*& cic, int checkLock,
	const String* list, bool mandatory, bool reverseRestrict)
{
    Lock lock(this);
    releaseCircuit(cic);
    if (!m_circuits)
	return false;
    if (list) {
	int s = -1;
	if (!mandatory && reverseRestrict) {
	    s = m_circuits->strategy();
	    // Use the opposite strategy restriction
	    if (s & SignallingCircuitGroup::OnlyEven)
		s = (s & ~SignallingCircuitGroup::OnlyEven) | SignallingCircuitGroup::OnlyOdd;
	    else if (s & SignallingCircuitGroup::OnlyOdd)
		s = (s & ~SignallingCircuitGroup::OnlyOdd) | SignallingCircuitGroup::OnlyEven;
	}
	cic = m_circuits->reserve(*list,mandatory,checkLock,s);
    }
    else
	cic = m_circuits->reserve(checkLock);
    return (cic != 0);
}

// Release a given circuit
bool SignallingCallControl::releaseCircuit(SignallingCircuit*& cic, bool sync)
{
    if (!cic)
	return false;
    bool ok = cic->status(SignallingCircuit::Idle,sync);
    cic->deref();
    cic = 0;
    return ok;
}

bool SignallingCallControl::releaseCircuit(unsigned int code, bool sync)
{
    Lock lock(this);
    SignallingCircuit* cic = m_circuits ? m_circuits->find(code) : 0;
    if (!cic)
	return false;
    return cic->status(SignallingCircuit::Idle,sync);
}

// Get events from calls
// Raise Disable event when no more calls and exiting
SignallingEvent* SignallingCallControl::getEvent(const Time& when)
{
    lock();
    ListIterator iter(m_calls);
    for (;;) {
	SignallingCall* call = static_cast<SignallingCall*>(iter.get());
	// End of iteration?
	if (!call)
	    break;
	RefPointer<SignallingCall> callRef = call;
	// Dead pointer?
	if (!callRef)
	    continue;
	unlock();
	SignallingEvent* event = callRef->getEvent(when);
	// Check if this call controller wants the event
	if (event && !processEvent(event))
	    return event;
	lock();
    }
    // Terminate if exiting and no more calls
    //TODO: Make sure we raise this event one time only
    if (exiting() && !m_calls.skipNull())
	return new SignallingEvent(SignallingEvent::Disable,0,this);
    unlock();
    return 0;
}

void SignallingCallControl::setDumper(SignallingDumper* dumper)
{
    Lock lock(this);
    if (m_dumper == dumper)
	return;
    SignallingDumper* tmp = m_dumper;
    m_dumper = dumper;
    delete tmp;
    XDebug(DebugAll,"SignallingCallControl. Data dumper set to (%p) [%p]",m_dumper,this);
}

// Clear call list
void SignallingCallControl::clearCalls()
{
    Lock lock(this);
    m_calls.clear();
}

// Remove a call from list
void SignallingCallControl::removeCall(SignallingCall* call, bool del)
{
    if (!call)
	return;
    Lock lock(this);
    if (m_calls.remove(call,del))
	DDebug(DebugAll,
	    "SignallingCallControl. Call (%p) removed from queue. Deleted: %s [%p]",
	    call,String::boolText(del),this);
}


/**
 * SignallingCall
 */
SignallingCall::SignallingCall(SignallingCallControl* controller, bool outgoing, bool signalOnly)
    : m_callMutex(true),
    m_lastEvent(0),
    m_controller(controller),
    m_outgoing(outgoing),
    m_signalOnly(signalOnly),
    m_inMsgMutex(true),
    m_private(0)
{
}

SignallingCall::~SignallingCall()
{
    m_inMsg.clear();
    if (m_controller)
	m_controller->removeCall(this,false);
}

// Event termination notification
void SignallingCall::eventTerminated(SignallingEvent* event)
{
    Lock lock(m_callMutex);
    if (!m_lastEvent || !event || m_lastEvent != event)
	return;
    XDebug(DebugAll,"SignallingCall. Event (%p,'%s') terminated [%p]",event,event->name(),this);
    m_lastEvent = 0;
}

void SignallingCall::enqueue(SignallingMessage* msg)
{
    if (!msg)
	return;
    Lock lock(m_inMsgMutex);
    m_inMsg.append(msg);
    XDebug(DebugAll,"SignallingCall. Enqueued message (%p,'%s') [%p]",
	msg,msg->name(),this);
}

// Dequeue a received message
SignallingMessage* SignallingCall::dequeue(bool remove)
{
    Lock lock(m_inMsgMutex);
    ObjList* obj = m_inMsg.skipNull();
    if (!obj)
	return 0;
    SignallingMessage* msg = static_cast<SignallingMessage*>(obj->get());
    if (remove) {
	m_inMsg.remove(msg,false);
	XDebug(DebugAll,"SignallingCall. Dequeued message (%p,'%s') [%p]",
	   msg,msg->name(),this);
    }
    return msg;
}


/**
 * SignallingEvent
 */
TokenDict SignallingEvent::s_types[] = {
	{"Unknown",  Unknown},
	{"Generic",  Generic},
	{"NewCall",  NewCall},
	{"Accept",   Accept},
	{"Connect",  Connect},
	{"Complete", Complete},
	{"Progress", Progress},
	{"Ringing",  Ringing},
	{"Answer",   Answer},
	{"Transfer", Transfer},
	{"Suspend",  Suspend},
	{"Resume",   Resume},
	{"Release",  Release},
	{"Info",     Info},
	{"Message",  Message},
	{"Facility", Facility},
	{"Enable",   Enable},
	{"Disable",  Disable},
	{"Reset",    Reset},
	{"Verify",   Verify},
	{0,0}
	};

SignallingEvent::SignallingEvent(Type type, SignallingMessage* message, SignallingCall* call)
    : m_type(type), m_message(0), m_call(0), m_controller(0)
{
    if (call && call->ref()) {
	m_call = call;
	m_controller = call->controller();
    }
    if (message && message->ref())
	m_message = message;
}

SignallingEvent::SignallingEvent(Type type, SignallingMessage* message, SignallingCallControl* controller)
    : m_type(type), m_message(0), m_call(0), m_controller(controller)
{
    if (message && message->ref())
	m_message = message;
}

SignallingEvent::~SignallingEvent()
{
    m_controller = 0;
    if (m_message)
	m_message->deref();
    if (m_call) {
	m_call->eventTerminated(this);
	m_call->deref();
    }
}


/**
 * SignallingCircuitEvent
 */
SignallingCircuitEvent::SignallingCircuitEvent(SignallingCircuit* cic, Type type, const char* name)
    : NamedList(name),
    m_circuit(0),
    m_type(type)
{
    XDebug(DebugAll,"SignallingCircuitEvent::SignallingCircuitEvent() [%p]",this);
    if (cic && cic->ref())
	m_circuit = cic;
}

SignallingCircuitEvent::~SignallingCircuitEvent()
{
    if (m_circuit) {
	m_circuit->eventTerminated(this);
	m_circuit->deref();
    }
    XDebug(DebugAll,"SignallingCircuitEvent::~SignallingCircuitEvent() [%p]",this);
}


/**
 * SignallingCircuit
 */
SignallingCircuit::SignallingCircuit(Type type, unsigned int code,
	SignallingCircuitGroup* group, SignallingCircuitSpan* span)
    : m_mutex(true),
    m_group(group),
    m_span(span),
    m_code(code),
    m_type(type),
    m_status(Disabled),
    m_lock(0),
    m_lastEvent(0)
{
    XDebug(m_group,DebugAll,"SignallingCircuit::SignallingCircuit [%p]",this);
}

SignallingCircuit::SignallingCircuit(Type type, unsigned int code, Status status,
	SignallingCircuitGroup* group, SignallingCircuitSpan* span)
    : m_mutex(true),
    m_group(group),
    m_span(span),
    m_code(code),
    m_type(type),
    m_status(status),
    m_lock(0),
    m_lastEvent(0)
{
    XDebug(m_group,DebugAll,"SignallingCircuit::SignallingCircuit [%p]",this);
}

SignallingCircuit::~SignallingCircuit()
{
    clearEvents();
    XDebug(m_group,DebugAll,"SignallingCircuit::~SignallingCircuit [%p]",this);
}

// Get first event from queue
SignallingCircuitEvent* SignallingCircuit::getEvent(const Time& when)
{
    Lock lock(m_mutex);
    if (m_lastEvent)
	return 0;
    ObjList* obj = m_events.skipNull();
    if (!obj)
	return 0;
    m_lastEvent = static_cast<SignallingCircuitEvent*>(m_events.remove(obj->get(),false));
    return m_lastEvent;
}

bool SignallingCircuit::sendEvent(SignallingCircuitEvent::Type type, NamedList* params)
{
    XDebug(m_group,DebugStub,"SignallingCircuit::sendEvent(%u,%p) [%p]",type,params,this);
    return false;
}

// Add event to queue
void SignallingCircuit::addEvent(SignallingCircuitEvent* event)
{
    if (!event)
	return;
    Lock lock(m_mutex);
    m_events.append(event);
}

// Clear event queue
void SignallingCircuit::clearEvents()
{
    Lock lock(m_mutex);
    m_events.clear();
}

// Event termination notification
void SignallingCircuit::eventTerminated(SignallingCircuitEvent* event)
{
    Lock lock(m_mutex);
    if (event && m_lastEvent == event) {
	XDebug(m_group,DebugAll,"Event (%p) '%s' terminated for cic=%u [%p]",
	    event,event->c_str(),code(),this);
	m_lastEvent = 0;
    }
}


/**
 * SignallingCircuitGroup
 */
TokenDict SignallingCircuitGroup::s_strategy[] = {
	{"increment", Increment},
	{"decrement", Decrement},
	{"lowest",    Lowest},
	{"highest",   Highest},
	{"random",    Random},
	{0,0}
	};

SignallingCircuitGroup::SignallingCircuitGroup(unsigned int base, int strategy, const char* name)
    : SignallingComponent(name),
    Mutex(true),
    m_base(base),
    m_last(0),
    m_strategy(strategy),
    m_used(0)
{
    setName(name);
    XDebug(this,DebugAll,"SignallingCircuitGroup::SignallingCircuitGroup() [%p]",this);
}

// Set circuits status to Missing. Clear circuit list
// Clear span list
SignallingCircuitGroup::~SignallingCircuitGroup()
{
    // Notify circuits of group destroy
    // Some of them may continue to exists after clearing the list
    Lock lock(this);
    ObjList* l = m_circuits.skipNull();
    for (; l; l = l->skipNext()) {
	SignallingCircuit* c = static_cast<SignallingCircuit*>(l->get());
	c->status(SignallingCircuit::Missing,true);
	c->m_group = 0;
    }
    m_circuits.clear();
    m_spans.clear();
    XDebug(this,DebugAll,"SignallingCircuitGroup::~SignallingCircuitGroup() [%p]",this);
}

// Find a circuit by code
SignallingCircuit* SignallingCircuitGroup::find(unsigned int cic, bool local)
{
    if (!local) {
	if (cic < m_base)
	    return 0;
	cic -= m_base;
    }
    Lock lock(this);
    if (cic >= m_last)
	return 0;
    ObjList* l = m_circuits.skipNull();
    for (; l; l = l->skipNext()) {
	SignallingCircuit* c = static_cast<SignallingCircuit*>(l->get());
	if (c->code() == cic)
	    return c;
    }
    return 0;
}

void SignallingCircuitGroup::getCicList(String& dest)
{
    dest = "";
    Lock lock(this);
    for (ObjList* l = m_circuits.skipNull(); l; l = l->skipNext()) {
	SignallingCircuit* c = static_cast<SignallingCircuit*>(l->get());
	dest.append(String(c->code()),",");
    }
}

// Insert a circuit if not already in the list
bool SignallingCircuitGroup::insert(SignallingCircuit* circuit)
{
    if (!circuit)
	return false;
    Lock lock(this);
    if (m_circuits.find(circuit) || find(circuit->code(),true))
	return false;
    m_circuits.append(circuit);
    if (m_last <= circuit->code())
	m_last = circuit->code() + 1;
    return true;
}

// Remove a circuit from list. Update maximum circuit code
void SignallingCircuitGroup::remove(SignallingCircuit* circuit)
{
    if (!circuit)
	return;
    Lock lock(this);
    if (!m_circuits.remove(circuit,false))
	return;
    // circuit was removed - rescan list for maximum cic
    m_last = 0;
    ObjList* l = m_circuits.skipNull();
    for (; l; l = l->skipNext()) {
	SignallingCircuit* c = static_cast<SignallingCircuit*>(l->get());
	if (m_last <= c->code())
	    m_last = c->code() + 1;
    }
}

// Append a span to the list if not already there
bool SignallingCircuitGroup::insertSpan(SignallingCircuitSpan* span)
{
    if (!span)
	return false;
    Lock lock(this);
    if (!m_spans.find(span))
	m_spans.append(span);
    return true;
}

// Remove a span from list
void SignallingCircuitGroup::removeSpan(SignallingCircuitSpan* span, bool delCics, bool delSpan)
{
    if (!span)
	return;
    Lock lock(this);
    if (delCics)
	removeSpanCircuits(span);
    m_spans.remove(span,delSpan);
}

// Remove circuits belonging to a span
void SignallingCircuitGroup::removeSpanCircuits(SignallingCircuitSpan* span)
{
    if (!span)
	return;
    Lock lock(this);
    ListIterator iter(m_circuits);
    for (GenObject* obj = 0; (obj = iter.get());) {
	SignallingCircuit* c = static_cast<SignallingCircuit*>(obj);
	if (span == c->span())
	    m_circuits.remove(c,true);
    }
}

// Get the status of a circuit given by its code
SignallingCircuit::Status SignallingCircuitGroup::status(unsigned int cic)
{
    Lock lock(this);
    SignallingCircuit* circuit = find(cic);
    return circuit ? circuit->status() : SignallingCircuit::Missing;
}

// Change the status of a circuit given by its code
bool SignallingCircuitGroup::status(unsigned int cic, SignallingCircuit::Status newStat,
	bool sync)
{
    Lock lock(this);
    SignallingCircuit* circuit = find(cic);
    return circuit && circuit->status(newStat,sync);
}

inline void adjustParity(unsigned int& n, int strategy)
{
    if ((strategy & SignallingCircuitGroup::OnlyEven) && (n & 1))
	n &= ~1;
    else if ((strategy & SignallingCircuitGroup::OnlyOdd) && !(n & 1))
	n |= 1;
}

// Choose the next circuit code to check, depending on strategy
unsigned int SignallingCircuitGroup::advance(unsigned int n, int strategy)
{
    // Increment by 2 when even or odd only circuits are requested
    unsigned int delta = (strategy & (OnlyOdd|OnlyEven)) ? 2 : 1;
    switch (strategy & 0xfff) {
	case Increment:
	case Lowest:
	    n += delta;
	    if (n >= m_last)
		n = delta;
	    break;
	case Decrement:
	case Highest:
	    if (n >= delta)
		n -= delta;
	    else {
		n = m_last - 1;
		adjustParity(n,strategy);
	    }
	    break;
	default:
	    n = (n + 1) % m_last;
	    break;
    }
    return n;
}

// Reserve a circuit
SignallingCircuit* SignallingCircuitGroup::reserve(int checkLock, int strategy)
{
    Lock lock(this);
    if (m_last < 1)
	return 0;
    if (strategy < 0)
	strategy = m_strategy;
    int dir = 1;
    unsigned int n = m_used;
    // first adjust the last used channel number
    switch (strategy & 0xfff) {
	case Increment:
	    n = (n + 1) % m_last;
	    break;
	case Decrement:
	    n = (n ? n : m_last) - 1;
	    dir = -1;
	    break;
	case Lowest:
	    n = 0;
	    break;
	case Highest:
	    n = m_last - 1;
	    dir = -1;
	    break;
	default:
	    while ((m_last > 1) && (n == m_used))
		n = ::random() % m_last;
    }
    // then go to the proper even/odd start circuit
    adjustParity(n,strategy);
    // remember where the scan started
    unsigned int start = n;
    // try at most how many channels we have, halve that if we only scan even or odd
    unsigned int i = m_last;
    if (strategy & (OnlyOdd|OnlyEven))
	i = (i + 1) / 2;
    while (i--) {
	SignallingCircuit* circuit = find(n,true);
	if (circuit && !circuit->locked(checkLock) && circuit->reserve()) {
	    if (circuit->ref()) {
		m_used = n;
		return circuit;
	    }
	    release(circuit);
	    return 0;
	}
	n = advance(n,strategy);
	// if wrapped around bail out, don't scan again
	if (n == start)
	    break;
    }
    lock.drop();
    if (strategy & Fallback) {
	if (strategy & OnlyEven) {
	    Debug(this,DebugNote,"No even circuits available, falling back to odd [%p]",this);
	    return reserve(checkLock,OnlyOdd | (strategy & 0xfff));
	}
	if (strategy & OnlyOdd) {
	    Debug(this,DebugNote,"No odd circuits available, falling back to even [%p]",this);
	    return reserve(checkLock,OnlyEven | (strategy & 0xfff));
	}
    }
    return 0;
}

// Reserve a circuit from the given list
// Reserve another one if not found and not mandatory
SignallingCircuit* SignallingCircuitGroup::reserve(const String& list, bool mandatory,
	int checkLock, int strategy)
{
    Lock lock(this);
    // Check if any of the given circuits are free
    while (true) {
	if (list.null())
	    break;
	ObjList* circuits = list.split(',',false);
	if (!circuits)
	    break;
	ObjList* obj = circuits->skipNull();
	SignallingCircuit* circuit = 0;
	for (; obj; obj = obj->skipNext()) {
	    int code = (static_cast<String*>(obj->get()))->toInteger(-1);
	    if (code == -1)
		continue;
	    circuit = find(code,false);
	    if (circuit && !circuit->locked(checkLock) && circuit->reserve()) {
		if (circuit->ref()) {
		    m_used = m_base + circuit->code();
		    TelEngine::destruct(circuits);
		    return circuit;
		}
		release(circuit);
	    }
	    circuit = 0;
	}
	TelEngine::destruct(circuits);
	break;
    }
    // Don't try to reserve another one if the given list is mandatory
    if (mandatory)
	return 0;
    return reserve(strategy,checkLock);
}

// Remove all spans and circuits. Release object
void SignallingCircuitGroup::destruct()
{
    lock();
    m_spans.clear();
    m_circuits.clear();
    unlock();
    GenObject::destruct();
}


/**
 * SignallingCircuitSpan
 */
SignallingCircuitSpan::SignallingCircuitSpan(const char* id, SignallingCircuitGroup* group)
    : m_group(group), m_id(id)
{
    if (m_group)
	m_group->insertSpan(this);
    XDebug(DebugAll,"SignallingCircuitSpan::SignallingCircuitSpan() '%s' [%p]",id,this);
}

SignallingCircuitSpan::~SignallingCircuitSpan()
{
    if (m_group)
	m_group->removeSpan(this,true,false);
    XDebug(DebugAll,"SignallingCircuitSpan::~SignallingCircuitSpan() '%s' [%p]",m_id.safe(),this);
}


/**
 * AnalogLine
 */
TokenDict AnalogLine::s_typeName[] = {
	{"FXO",     FXO},
	{"FXS",     FXS},
	{"monitor", Monitor},
	{0,0}
	};

TokenDict AnalogLine::s_stateName[] = {
	{"OutOfService",   OutOfService},
	{"Idle",           Idle},
	{"Dialing",        Dialing},
	{"DialComplete",   DialComplete},
	{"Ringing",        Ringing},
	{"Answered",       Answered},
	{"CallEnded",      CallEnded},
	{"OutOfOrder",     OutOfOrder},
	{0,0}
	};

TokenDict AnalogLine::s_csName[] = {
	{"after",  After},
	{"before", Before},
	{"none",   NoCallSetup},
	{0,0}
	};

inline u_int64_t getValidInt(const NamedList& params, const char* param, int defVal)
{
    int tmp = params.getIntValue(param,defVal);
    return tmp >= 0 ? tmp : defVal;
}

// Reserve the line's circuit
AnalogLine::AnalogLine(AnalogLineGroup* grp, unsigned int cic, const NamedList& params)
    : Mutex(true),
    m_type(Unknown),
    m_state(Idle),
    m_inband(false),
    m_echocancel(0),
    m_acceptPulseDigit(true),
    m_answerOnPolarity(false),
    m_hangupOnPolarity(false),
    m_polarityControl(false),
    m_callSetup(NoCallSetup),
    m_callSetupTimeout(0),
    m_noRingTimeout(0),
    m_alarmTimeout(0),
    m_group(grp),
    m_circuit(0),
    m_private(0),
    m_peer(0),
    m_getPeerEvent(false)
{
    // Check and set some data
    const char* error = 0;
    while (true) {
#define CHECK_DATA(test,sError) if (test) { error = sError; break; }
	CHECK_DATA(!m_group,"circuit group is missing")
	CHECK_DATA(m_group->findLine(cic),"circuit already allocated")
	SignallingCircuit* circuit = m_group->find(cic);
	if (circuit && circuit->ref())
	    m_circuit = circuit;
	CHECK_DATA(!m_circuit,"circuit is missing")
	break;
#undef CHECK_DATA
    }
    if (error) {
	Debug(m_group,DebugNote,"Can't create analog line (cic=%u): %s",
	    cic,error);
	return;
    }

    m_type = m_group->type();
    m_address << m_group->toString() << "/" << m_circuit->code();
    m_inband = params.getBoolValue("dtmfinband",false);
    String tmp = params.getValue("echocancel");
    if (tmp.isBoolean())
	m_echocancel = tmp.toBoolean() ? 1 : -1;
    m_answerOnPolarity = params.getBoolValue("answer-on-polarity",false);
    m_hangupOnPolarity = params.getBoolValue("hangup-on-polarity",false);
    m_polarityControl = params.getBoolValue("polaritycontrol",false);

    m_callSetup = (CallSetupInfo)lookup(params.getValue("callsetup"),s_csName,After);

    m_callSetupTimeout = getValidInt(params,"callsetup-timeout",2000);
    m_noRingTimeout = getValidInt(params,"ring-timeout",10000);
    m_alarmTimeout = getValidInt(params,"alarm-timeout",30000);
    m_delayDial = getValidInt(params,"delaydial",2000);

    DDebug(m_group,DebugAll,"AnalogLine() addr=%s type=%s [%p]",
	address(),lookup(m_type,s_typeName),this);

    if (!params.getBoolValue("out-of-service",false)) {
	resetCircuit();
	if (params.getBoolValue("connect",true))
	    connect(false);
    }
    else
	enable(false,false);
}

AnalogLine::~AnalogLine()
{
    DDebug(m_group,DebugAll,"~AnalogLine() addr=%s [%p]",address(),this);
}

// Remove old peer's peer. Set this line's peer
void AnalogLine::setPeer(AnalogLine* line, bool sync)
{
    Lock lock(this);
    if (line == this) {
	Debug(m_group,DebugNote,"%s: Attempt to set peer to itself [%p]",
		address(),this);
	return;
    }
    if (line == m_peer) {
	if (sync && m_peer) {
	    XDebug(m_group,DebugAll,"%s: Syncing with peer (%p) '%s' [%p]",
		address(),m_peer,m_peer->address(),this);
	    m_peer->setPeer(this,false);
	}
	return;
    }
    AnalogLine* tmp = m_peer;
    m_peer = 0;
    if (tmp) {
	DDebug(m_group,DebugAll,"%s: Removed peer (%p) '%s' [%p]",
	    address(),tmp,tmp->address(),this);
	if (sync)
	    tmp->setPeer(0,false);
    }
    m_peer = line;
    if (m_peer) {
	DDebug(m_group,DebugAll,"%s: Peer set to (%p) '%s' [%p]",
	    address(),m_peer,m_peer->address(),this);
	if (sync)
	    m_peer->setPeer(this,false);
    }
}

// Reset the line circuit's echo canceller to line default echo canceller state
void AnalogLine::resetEcho(bool train)
{
    if (!(m_circuit || m_echocancel))
	return;
    bool enable = (m_echocancel > 0);
    m_circuit->setParam("echocancel",String::boolText(enable));
    if (enable && train)
	m_circuit->setParam("echotrain",String(""));
}

// Connect the line's circuit. Reset line echo canceller
bool AnalogLine::connect(bool sync)
{
    Lock lock(this);
    bool ok = m_circuit && m_circuit->connect();
    resetEcho(true);
    if (sync && ok && m_peer)
	m_peer->connect(false);
    return ok;
}

// Disconnect the line's circuit. Reset line echo canceller
bool AnalogLine::disconnect(bool sync)
{
    Lock lock(this);
    bool ok = m_circuit && m_circuit->disconnect();
    resetEcho(false);
    if (sync && ok && m_peer)
	m_peer->disconnect(false);
    return ok;
}

// Send an event through this line
bool AnalogLine::sendEvent(SignallingCircuitEvent::Type type, NamedList* params)
{
    Lock lock(this);
    if (state() == OutOfService)
	return false;
    if (m_inband &&
	(type == SignallingCircuitEvent::Dtmf || type == SignallingCircuitEvent::PulseDigit))
	return false;
    return m_circuit && m_circuit->sendEvent(type,params);
}

// Get events from the line's circuit if not out of service
AnalogLineEvent* AnalogLine::getEvent(const Time& when)
{
    Lock lock(this);
    if (state() == OutOfService) {
	checkTimeouts(when);
	return 0;
    }

    SignallingCircuitEvent* event = m_circuit ? m_circuit->getEvent(when) : 0;
    if (!event) {
	checkTimeouts(when);
	return 0;
    }

    if ((event->type() == SignallingCircuitEvent::PulseDigit ||
	event->type() == SignallingCircuitEvent::PulseStart) &&
	!m_acceptPulseDigit) {
	DDebug(m_group,DebugInfo,"%s: ignoring pulse event '%s' [%p]",
	    address(),event->c_str(),this);
	delete event;
	return 0;
    }

    return new AnalogLineEvent(this,event);
}

// Alternate get events from this line or peer
AnalogLineEvent* AnalogLine::getMonitorEvent(const Time& when)
{
    Lock lock(this);
    m_getPeerEvent = !m_getPeerEvent;
    AnalogLineEvent* event = 0;
    if (m_getPeerEvent) {
	event = getEvent(when);
	if (!event && m_peer)
	    event = m_peer->getEvent(when);
    }
    else {
	if (m_peer)
	    event = m_peer->getEvent(when);
	if (!event)
	    event = getEvent(when);
    }
    return event;
}

// Change the line state if neither current or new state are OutOfService
bool AnalogLine::changeState(State newState, bool sync)
{
    Lock lock(this);
    bool ok = false;
    while (true) {
	if (m_state == newState || m_state == OutOfService || newState == OutOfService)
	    break;
	if (newState != Idle && newState < m_state)
	    break;
	DDebug(m_group,DebugInfo,"%s: changed state from %s to %s [%p]",
	    address(),lookup(m_state,s_stateName),
	    lookup(newState,s_stateName),this);
	m_state = newState;
	ok = true;
	break;
    }
    if (sync && ok && m_peer)
	m_peer->changeState(newState,false);
    return true;
}

// Enable/disable line. Change circuit's state to Disabled/Reserved when
//  entering/exiting the OutOfService state
bool AnalogLine::enable(bool ok, bool sync, bool connectNow)
{
    Lock lock(this);
    while (true) {
	if (ok) {
	    if (m_state != OutOfService)
		break;
	    Debug(m_group,DebugInfo,"%s: back in service [%p]",address(),this);
	    m_state = Idle;
	    if (m_circuit) {
		m_circuit->status(SignallingCircuit::Reserved);
		if (connectNow)
		    connect(false);
	    }
	    break;
	}
	// Disable
	if (m_state == OutOfService)
	    break;
	Debug(m_group,DebugNote,"%s: out of service [%p]",address(),this);
	m_state = OutOfService;
	disconnect(false);
	if (m_circuit)
	    m_circuit->status(SignallingCircuit::Disabled);
	break;
    }
    if (sync && m_peer)
	m_peer->enable(ok,false,connectNow);
    return true;
}

// Deref the circuit
void AnalogLine::destroyed()
{
    lock();
    disconnect(false);
    if (m_circuit)
	m_circuit->status(SignallingCircuit::Idle);
    setPeer(0,true);
    if (m_group)
	m_group->removeLine(this);
    TelEngine::destruct(m_circuit);
    unlock();
    RefObject::destroyed();
}


/**
 * AnalogLineGroup
 */
// Construct an analog line group owning single lines
AnalogLineGroup::AnalogLineGroup(AnalogLine::Type type, const char* name, bool slave)
    : SignallingCircuitGroup(0,SignallingCircuitGroup::Increment,name),
    m_type(type),
    m_fxo(0),
    m_slave(false)
{
    setName(name);
    if (m_type == AnalogLine::FXO)
	m_slave = slave;
    XDebug(this,DebugAll,"AnalogLineGroup() [%p]",this);
}

// Constructs an FXS analog line monitor
AnalogLineGroup::AnalogLineGroup(const char* name, AnalogLineGroup* fxo)
    : SignallingCircuitGroup(0,SignallingCircuitGroup::Increment,name),
    m_type(AnalogLine::FXS),
    m_fxo(fxo)
{
    setName(name);
    if (m_fxo)
	m_fxo->debugChain(this);
    else
	Debug(this,DebugWarn,"Request to create monitor without fxo group [%p]",this);
    XDebug(this,DebugAll,"AnalogLineGroup() monitor fxo=%p [%p]",m_fxo,this);
}

AnalogLineGroup::~AnalogLineGroup()
{
    XDebug(this,DebugAll,"~AnalogLineGroup() [%p]",this);
}

// Append it to the list
bool AnalogLineGroup::appendLine(AnalogLine* line, bool destructOnFail)
{
    if (!(line && line->type() == m_type && line->group() == this)) {
	if (destructOnFail)
	    TelEngine::destruct(line);
	return false;
    }
    Lock lock(this);
    m_lines.append(line);
    DDebug(this,DebugAll,"Added line (%p) %s [%p]",line,line->address(),this);
    return true;
}

// Remove a line from the list and destruct it
void AnalogLineGroup::removeLine(unsigned int cic)
{
    Lock lock(this);
    AnalogLine* line = findLine(cic);
    if (!line)
	return;
    removeLine(line);
    TelEngine::destruct(line);
}

// Remove a line from the list without destroying it
void AnalogLineGroup::removeLine(AnalogLine* line)
{
    if (!line)
	return;
    Lock lock(this);
    if (m_lines.remove(line,false))
	DDebug(this,DebugAll,"Removed line %p %s [%p]",line,line->address(),this);
}

// Find a line by its circuit
AnalogLine* AnalogLineGroup::findLine(unsigned int cic)
{
    Lock lock(this);
    for (ObjList* o = m_lines.skipNull(); o; o = o->skipNext()) {
	AnalogLine* line = static_cast<AnalogLine*>(o->get());
	if (line->circuit() && line->circuit()->code() == cic)
	    return line;
    }
    return 0;
}

// Find a line by its address
AnalogLine* AnalogLineGroup::findLine(const String& address)
{
    Lock lock(this);
    ObjList* tmp = m_lines.find(address);
    return tmp ? static_cast<AnalogLine*>(tmp->get()) : 0;
}

// Iterate through the line list to get an event
AnalogLineEvent* AnalogLineGroup::getEvent(const Time& when)
{
    lock();
    ListIterator iter(m_lines);
    for (;;) {
	AnalogLine* line = static_cast<AnalogLine*>(iter.get());
	// End of iteration?
	if (!line)
	    break;
	RefPointer<AnalogLine> lineRef = line;
	// Dead pointer?
	if (!lineRef)
	    continue;
	unlock();
	AnalogLineEvent* event = !fxo() ? lineRef->getEvent(when) : lineRef->getMonitorEvent(when);
	if (event)
	    return event;
	lock();
    }
    unlock();
    return 0;
}

// Remove all spans and circuits. Release object
void AnalogLineGroup::destruct()
{
    lock();
    for (ObjList* o = m_lines.skipNull(); o; o = o->skipNext()) {
	AnalogLine* line = static_cast<AnalogLine*>(o->get());
	Lock lock(line);
	line->m_group = 0;
    }
    m_lines.clear();
    TelEngine::destruct(m_fxo);
    unlock();
    SignallingCircuitGroup::destruct();
}

/* vi: set ts=8 sw=4 sts=4 noet: */
