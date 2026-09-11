/**
SPDX-License-Identifier: Apache-2.0

FastFileLink CLI - Fast, no-fuss file sharing
Copyright (C) 2025-2026 FastFileLink contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pythread.h>

#include "Agent.h"
#include "Juice.h"
#include "Plum.h"

#ifdef FFL_P2P_HAVE_QUIC
#include "Quic.h"
#endif

#include <stdlib.h>
#include <string.h>

#ifdef _PyCFunction_CAST
#define FFL_PYC_FUNCTION_CAST(function) _PyCFunction_CAST(function)
#else
#define FFL_PYC_FUNCTION_CAST(function) (PyCFunction)(void (*)(void))(function)
#endif

typedef enum EventKind {
    EVENT_STATE = 1,
    EVENT_CANDIDATE,
    EVENT_GATHERING_DONE,
    EVENT_RECEIVE
} EventKind;

typedef struct EventNode {
    EventKind kind;
    int state;
    char *text;
    unsigned char *data;
    size_t size;
    struct EventNode *next;
} EventNode;

typedef struct ReceiveNode {
    unsigned char *data;
    size_t size;
    struct ReceiveNode *next;
} ReceiveNode;

typedef struct PyP2PAgent {
    PyObject_HEAD
    FFLP2PJuice *juice;
    PyThread_type_lock lock;
    PyThread_type_lock eventAvailable;
    PyThread_type_lock receiveAvailable;
    int eventSignaled;
    int receiveSignaled;
    EventNode *head;
    EventNode *tail;
    ReceiveNode *receiveHead;
    ReceiveNode *receiveTail;
    FFLP2PAgentDataSink dataSink;
    void *dataSinkUserPtr;
    int closed;
} PyP2PAgent;

static PyTypeObject *gAgentType = NULL;

static int parseNativeLogLevel(const char *name, int *level) {
    if (strcmp(name, "verbose") == 0) {
        *level = JUICE_LOG_LEVEL_VERBOSE;
    } else if (strcmp(name, "debug") == 0) {
        *level = JUICE_LOG_LEVEL_DEBUG;
    } else if (strcmp(name, "info") == 0) {
        *level = JUICE_LOG_LEVEL_INFO;
    } else if (strcmp(name, "warning") == 0 || strcmp(name, "warn") == 0) {
        *level = JUICE_LOG_LEVEL_WARN;
    } else if (strcmp(name, "error") == 0) {
        *level = JUICE_LOG_LEVEL_ERROR;
    } else if (strcmp(name, "fatal") == 0) {
        *level = JUICE_LOG_LEVEL_FATAL;
    } else if (strcmp(name, "none") == 0) {
        *level = JUICE_LOG_LEVEL_NONE;
    } else {
        return -1;
    }

    return 0;
}

static PyObject *setLogLevel(PyObject *self, PyObject *args) {
    const char *name;
    int level;

    (void)self;
    if (!PyArg_ParseTuple(args, "s:setLogLevel", &name))
        return NULL;

    if (parseNativeLogLevel(name, &level) < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "log level must be verbose, debug, info, warning, error, fatal, or none");
        return NULL;
    }

    setFFLP2PJuiceLogLevel((juice_log_level_t)level);
    setFFLP2PPortMappingLogLevel((plum_log_level_t)level);

    Py_RETURN_NONE;
}

static int setDictionaryString(PyObject *dictionary, const char *key, const char *value) {
    PyObject *object = PyUnicode_FromString(value);
    int result;

    if (!object)
        return -1;

    result = PyDict_SetItemString(dictionary, key, object);
    Py_DECREF(object);

    return result;
}

static int setDictionaryUInt64(PyObject *dictionary, const char *key, uint64_t value) {
    PyObject *object = PyLong_FromUnsignedLongLong((unsigned long long)value);
    int result;

    if (!object)
        return -1;

    result = PyDict_SetItemString(dictionary, key, object);
    Py_DECREF(object);

    return result;
}


static char *duplicateString(const char *text) {
    size_t size;
    char *copy;

    if (!text)
        return NULL;

    size = strlen(text) + 1;
    copy = (char *)malloc(size);
    if (copy)
        memcpy(copy, text, size);

    return copy;
}

static void freeEvent(EventNode *event) {
    if (!event)
        return;

    free(event->text);
    free(event->data);
    free(event);
}

static void queueEvent(PyP2PAgent *self, EventNode *event) {
    int signal = 0;

    if (!event)
        return;

    if (!self || !self->lock || !self->eventAvailable || self->closed) {
        freeEvent(event);
        return;
    }

    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    if (self->closed) {
        PyThread_release_lock(self->lock);
        freeEvent(event);
        return;
    }

    if (self->tail)
        self->tail->next = event;
    else
        self->head = event;

    self->tail = event;
    if (!self->eventSignaled) {
        self->eventSignaled = 1;
        signal = 1;
    }

    PyThread_release_lock(self->lock);

    if (signal)
        PyThread_release_lock(self->eventAvailable);
}

static void onState(void *user, juice_state_t state) {
    PyP2PAgent *self = (PyP2PAgent *)user;
    EventNode *event = (EventNode *)calloc(1, sizeof(*event));

    if (!event)
        return;

    event->kind = EVENT_STATE;
    event->state = (int)state;
    queueEvent(self, event);
}

static void onCandidate(void *user, const char *candidateSDP) {
    PyP2PAgent *self = (PyP2PAgent *)user;

    EventNode *event = (EventNode *)calloc(1, sizeof(*event));
    if (!event)
        return;

    event->kind = EVENT_CANDIDATE;
    event->text = duplicateString(candidateSDP ? candidateSDP : "");
    if (!event->text) {
        freeEvent(event);
        return;
    }

    queueEvent(self, event);
}

static void onGatheringDone(void *user) {
    PyP2PAgent *self = (PyP2PAgent *)user;

    EventNode *event = (EventNode *)calloc(1, sizeof(*event));
    if (!event)
        return;

    event->kind = EVENT_GATHERING_DONE;

    queueEvent(self, event);
}

static void freeReceive(ReceiveNode *receive) {
    if (!receive)
        return;

    free(receive->data);
    free(receive);
}

static void queueReceive(PyP2PAgent *self, ReceiveNode *receive) {
    int signalEvent = 0;
    int signalReceive = 0;
    if (!receive)
        return;

    if (!self || !self->lock || !self->eventAvailable || !self->receiveAvailable || self->closed) {
        freeReceive(receive);
        return;
    }

    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    if (self->closed) {
        PyThread_release_lock(self->lock);
        freeReceive(receive);
        return;
    }

    if (self->receiveTail)
        self->receiveTail->next = receive;
    else
        self->receiveHead = receive;

    self->receiveTail = receive;
    if (!self->receiveSignaled) {
        self->receiveSignaled = 1;
        signalReceive = 1;
    }

    if (!self->eventSignaled) {
        self->eventSignaled = 1;
        signalEvent = 1;
    }

    PyThread_release_lock(self->lock);

    if (signalReceive)
        PyThread_release_lock(self->receiveAvailable);

    if (signalEvent)
        PyThread_release_lock(self->eventAvailable);
}

static void onReceive(void *user, const void *data, size_t size) {
    PyP2PAgent *self = (PyP2PAgent *)user;
    FFLP2PAgentDataSink sink = NULL;
    void *sinkUserPtr = NULL;
    ReceiveNode *receive;

    if (!self || !self->lock)
        return;

    /*
     * The native QUIC transport installs a data sink while it owns this ICE path.
     * Invoke it under the agent lock so detach is a synchronization point:
     * once clearFFLP2PAgentNativeDataSink() returns, no callback can still be
     * using the runtime object. The sink only queues native work and never
     * calls back into the Agent, so holding this lock cannot recurse.
     */
    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    if (!self->closed && self->dataSink) {
        sink = self->dataSink;
        sinkUserPtr = self->dataSinkUserPtr;
        sink(sinkUserPtr, data, size);
        PyThread_release_lock(self->lock);
        return;
    }
    PyThread_release_lock(self->lock);

    receive = (ReceiveNode *)calloc(1, sizeof(*receive));
    if (!receive)
        return;

    if (size) {
        receive->data = (unsigned char *)malloc(size);
        if (!receive->data) {
            freeReceive(receive);
            return;
        }

        memcpy(receive->data, data, size);
    }

    receive->size = size;
    queueReceive(self, receive);
}

void *getFFLP2PAgentNativeHandle(PyObject *agentObject) {
    if (!gAgentType || !agentObject || !PyObject_TypeCheck(agentObject, gAgentType)) {
        PyErr_SetString(PyExc_TypeError, "agent must be an _ffl_p2p.Agent");
        return NULL;
    }

    return (void *)agentObject;
}


int setFFLP2PAgentNativeDataSink(void *agentHandle, FFLP2PAgentDataSink sink, void *userPtr) {
    PyP2PAgent *self = (PyP2PAgent *)agentHandle;
    if (!self || !sink || !self->lock)
        return -1;

    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    if (self->closed || !self->juice || self->dataSink) {
        PyThread_release_lock(self->lock);
        return -1;
    }

    self->dataSink = sink;
    self->dataSinkUserPtr = userPtr;
    PyThread_release_lock(self->lock);

    return 0;
}

void clearFFLP2PAgentNativeDataSink(void *agentHandle, FFLP2PAgentDataSink sink, void *userPtr) {
    PyP2PAgent *self = (PyP2PAgent *)agentHandle;
    if (!self || !self->lock)
        return;

    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    if (self->dataSink == sink && self->dataSinkUserPtr == userPtr) {
        self->dataSink = NULL;
        self->dataSinkUserPtr = NULL;
    }

    PyThread_release_lock(self->lock);
}

int sendFFLP2PAgentNative(void *agentHandle, const void *data, size_t size) {
    PyP2PAgent *self = (PyP2PAgent *)agentHandle;
    if (!self || self->closed || !self->juice)
        return JUICE_ERR_INVALID;

    return sendFFLP2PJuice(self->juice, data, size);
}

int sendFFLP2PAgentNativeAggregateProgress(void *agentHandle, const void *data, size_t size,
                                           size_t segmentSize, size_t *sentSize) {
    PyP2PAgent *self = (PyP2PAgent *)agentHandle;
    if (!self || self->closed || !self->juice || !sentSize)
        return JUICE_ERR_INVALID;

    return sendFFLP2PJuiceAggregateProgress(self->juice, data, size, segmentSize, sentSize);
}

static ReceiveNode *popReceiveLocked(PyP2PAgent *self, int *signalAgain) {
    ReceiveNode *receive = self->receiveHead;
    if (!receive)
        return NULL;

    self->receiveHead = receive->next;
    if (!self->receiveHead)
        self->receiveTail = NULL;

    receive->next = NULL;

    /* Consume the old binary wake token. Re-signal below when more data remains. */
    if (self->receiveSignaled &&
        PyThread_acquire_lock(self->receiveAvailable, NOWAIT_LOCK)) {
        self->receiveSignaled = 0;
    }

    if (self->receiveHead && !self->receiveSignaled) {
        self->receiveSignaled = 1;
        *signalAgain = 1;
    }

    return receive;
}

int receiveFFLP2PAgentNative(void *agentHandle, double timeout,
                             unsigned char **data, size_t *size) {
    PyP2PAgent *self = (PyP2PAgent *)agentHandle;
    ReceiveNode *receive = NULL;
    int signalAgain = 0;

    if (!data || !size || !self)
        return -1;

    *data = NULL;
    *size = 0;
    if (timeout < 0.0)
        return -1;

    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    receive = popReceiveLocked(self, &signalAgain);
    PyThread_release_lock(self->lock);
    if (signalAgain)
        PyThread_release_lock(self->receiveAvailable);

    if (!receive && timeout > 0.0) {
        const PY_TIMEOUT_T timeoutUs = (PY_TIMEOUT_T)(timeout * 1000000.0);
        const PyLockStatus waitStatus =
            PyThread_acquire_lock_timed(self->receiveAvailable, timeoutUs, 0);

        if (waitStatus == PY_LOCK_ACQUIRED) {
            PyThread_acquire_lock(self->lock, WAIT_LOCK);
            self->receiveSignaled = 0;
            receive = popReceiveLocked(self, &signalAgain);
            PyThread_release_lock(self->lock);

            if (signalAgain)
                PyThread_release_lock(self->receiveAvailable);
        }
    }

    if (!receive)
        return 0;

    *data = receive->data;
    *size = receive->size;
    receive->data = NULL;
    freeReceive(receive);

    return 1;
}

void freeFFLP2PAgentNativeReceive(unsigned char *data) {
    free(data);
}

static int checkJuiceResult(int result, const char *operation) {
    if (result == JUICE_ERR_SUCCESS)
        return 1;

    PyErr_Format(PyExc_RuntimeError, "%s failed: libjuice error %d", operation, result);
    return 0;
}

static int requireOpen(PyP2PAgent *self) {
    if (!self->juice || self->closed) {
        PyErr_SetString(PyExc_RuntimeError, "agent is closed");
        return 0;
    }

    return 1;
}

static int initializeAgent(PyP2PAgent *self, PyObject *args, PyObject *kwargs) {
    const char *stunHost = NULL;
    unsigned int stunPort = 3478;
    const char *bindAddress = NULL;
    unsigned int portBegin = 0;
    unsigned int portEnd = 0;
    static char *keywordList[] = {"stunHost", "stunPort", "bindAddress", "portBegin", "portEnd", NULL};
    FFLP2PJuiceConfig config;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|zIzII", keywordList,
                                     &stunHost, &stunPort, &bindAddress,
                                     &portBegin, &portEnd))
        return -1;

    if (stunPort > 65535 || portBegin > 65535 || portEnd > 65535) {
        PyErr_SetString(PyExc_ValueError, "port out of range");
        return -1;
    }

    self->lock = PyThread_allocate_lock();
    self->eventAvailable = PyThread_allocate_lock();
    self->receiveAvailable = PyThread_allocate_lock();
    if (!self->lock || !self->eventAvailable || !self->receiveAvailable) {
        if (self->receiveAvailable) {
            PyThread_free_lock(self->receiveAvailable);
            self->receiveAvailable = NULL;
        }

        if (self->eventAvailable) {
            PyThread_free_lock(self->eventAvailable);
            self->eventAvailable = NULL;
        }

        if (self->lock) {
            PyThread_free_lock(self->lock);
            self->lock = NULL;
        }

        PyErr_NoMemory();

        return -1;
    }

    /* Binary events: locked means no queued event/datagram. */
    PyThread_acquire_lock(self->eventAvailable, WAIT_LOCK);
    PyThread_acquire_lock(self->receiveAvailable, WAIT_LOCK);

    memset(&config, 0, sizeof(config));

    config.stunHost = stunHost;
    config.stunPort = (uint16_t)stunPort;
    config.bindAddress = bindAddress;
    config.portBegin = (uint16_t)portBegin;
    config.portEnd = (uint16_t)portEnd;
    config.user = self;
    config.onState = onState;
    config.onCandidate = onCandidate;
    config.onGatheringDone = onGatheringDone;
    config.onReceive = onReceive;

    self->juice = createFFLP2PJuice(&config);
    if (!self->juice) {
        PyThread_free_lock(self->receiveAvailable);
        self->receiveAvailable = NULL;
        PyThread_free_lock(self->eventAvailable);
        self->eventAvailable = NULL;
        PyThread_free_lock(self->lock);
        self->lock = NULL;
        PyErr_SetString(PyExc_RuntimeError, "createFFLP2PJuice failed");

        return -1;
    }

    return 0;
}

static void deallocateAgent(PyP2PAgent *self) {
    EventNode *event;
    ReceiveNode *receive;

    if (self->lock) {
        PyThread_acquire_lock(self->lock, WAIT_LOCK);
        self->closed = 1;
        self->dataSink = NULL;
        self->dataSinkUserPtr = NULL;
        PyThread_release_lock(self->lock);
    }

    if (self->juice) {
        destroyFFLP2PJuice(self->juice);
        self->juice = NULL;
    }

    if (self->lock) {
        PyThread_acquire_lock(self->lock, WAIT_LOCK);
        event = self->head;
        receive = self->receiveHead;
        self->head = NULL;
        self->tail = NULL;
        self->receiveHead = NULL;
        self->receiveTail = NULL;
        PyThread_release_lock(self->lock);

        while (event) {
            EventNode *next = event->next;
            freeEvent(event);
            event = next;
        }

        while (receive) {
            ReceiveNode *next = receive->next;
            freeReceive(receive);
            receive = next;
        }

        PyThread_free_lock(self->lock);
        self->lock = NULL;
    }

    if (self->receiveAvailable) {
        PyThread_free_lock(self->receiveAvailable);
        self->receiveAvailable = NULL;
    }

    if (self->eventAvailable) {
        PyThread_free_lock(self->eventAvailable);
        self->eventAvailable = NULL;
    }

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *closeAgent(PyP2PAgent *self, PyObject *Py_UNUSED(ignored)) {
    if (!self->closed && self->lock) {
        PyThread_acquire_lock(self->lock, WAIT_LOCK);
        self->closed = 1;
        self->dataSink = NULL;
        self->dataSinkUserPtr = NULL;
        PyThread_release_lock(self->lock);
    }

    if (self->juice) {
        destroyFFLP2PJuice(self->juice);
        self->juice = NULL;
    }

    Py_RETURN_NONE;
}

#define SIMPLE_AGENT_METHOD(name, function, label) \
static PyObject *name(PyP2PAgent *self, PyObject *Py_UNUSED(ignored)) { \
    if (!requireOpen(self)) return NULL; \
    if (!checkJuiceResult(function(self->juice), label)) return NULL; \
    Py_RETURN_NONE; \
}

SIMPLE_AGENT_METHOD(gatherAgent, gatherFFLP2PJuice, "gather")
SIMPLE_AGENT_METHOD(holdAgentGathering, holdFFLP2PJuiceGathering, "holdGathering")
SIMPLE_AGENT_METHOD(releaseAgentGathering, releaseFFLP2PJuiceGathering, "releaseGathering")
SIMPLE_AGENT_METHOD(completeAgentRemoteGathering, completeFFLP2PJuiceRemoteGathering, "remoteGatheringDone")

static PyObject *getAgentLocalPort(PyP2PAgent *self, PyObject *Py_UNUSED(ignored)) {
    uint16_t port = 0;

    if (!requireOpen(self))
        return NULL;

    if (!checkJuiceResult(getFFLP2PJuiceLocalPort(self->juice, &port), "localPort"))
        return NULL;

    return PyLong_FromUnsignedLong(port);
}

static PyObject *getAgentLocalDescription(PyP2PAgent *self, PyObject *Py_UNUSED(ignored)) {
    char buffer[JUICE_MAX_SDP_STRING_LEN];

    if (!requireOpen(self))
        return NULL;

    if (!checkJuiceResult(getFFLP2PJuiceLocalDescription(self->juice, buffer, sizeof(buffer)), "localDescription"))
        return NULL;

    return PyUnicode_FromString(buffer);
}

static PyObject *setAgentRemoteDescription(PyP2PAgent *self, PyObject *argument) {
    const char *sdp;

    if (!requireOpen(self))
        return NULL;

    sdp = PyUnicode_AsUTF8(argument);
    if (!sdp)
        return NULL;

    if (!checkJuiceResult(setFFLP2PJuiceRemoteDescription(self->juice, sdp), "setRemoteDescription"))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *addAgentRemoteCandidate(PyP2PAgent *self, PyObject *argument) {
    const char *candidateSDP;
    if (!requireOpen(self))
        return NULL;

    candidateSDP = PyUnicode_AsUTF8(argument);
    if (!candidateSDP)
        return NULL;

    if (!checkJuiceResult(addFFLP2PJuiceRemoteCandidate(self->juice, candidateSDP), "addRemoteCandidate"))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *addAgentMappedCandidate(PyP2PAgent *self, PyObject *args) {
    const char *address;
    unsigned int port;
    if (!requireOpen(self))
        return NULL;

    if (!PyArg_ParseTuple(args, "sI", &address, &port))
        return NULL;

    if (!port || port > 65535) {
        PyErr_SetString(PyExc_ValueError, "port out of range");
        return NULL;
    }

    if (!checkJuiceResult(addFFLP2PJuiceMappedCandidate(self->juice, address, (uint16_t)port),
                          "addMappedCandidate"))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *sendAgent(PyP2PAgent *self, PyObject *argument) {
    Py_buffer view;
    int result;

    if (!requireOpen(self))
        return NULL;

    if (PyObject_GetBuffer(argument, &view, PyBUF_CONTIG_RO) < 0)
        return NULL;

    result = sendFFLP2PJuice(self->juice, view.buf, (size_t)view.len);

    PyBuffer_Release(&view);
    if (!checkJuiceResult(result, "send"))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *getAgentState(PyP2PAgent *self, PyObject *Py_UNUSED(ignored)) {
    if (!requireOpen(self))
        return NULL;

    return PyLong_FromLong((long)getFFLP2PJuiceState(self->juice));
}

static PyObject *getAgentSelectedAddresses(PyP2PAgent *self, PyObject *Py_UNUSED(ignored)) {
    char local[JUICE_MAX_ADDRESS_STRING_LEN] = {0};
    char remote[JUICE_MAX_ADDRESS_STRING_LEN] = {0};
    int result;

    if (!requireOpen(self))
        return NULL;

    result = getFFLP2PJuiceSelectedAddresses(self->juice, local, sizeof(local), remote, sizeof(remote));
    if (result == JUICE_ERR_NOT_AVAIL)
        Py_RETURN_NONE;

    if (!checkJuiceResult(result, "selectedAddresses"))
        return NULL;

    return Py_BuildValue("(ss)", local, remote);
}

static PyObject *convertEventToPython(EventNode *event) {
    PyObject *dictionary = PyDict_New();
    PyObject *type = NULL;
    PyObject *value = NULL;

    if (!dictionary)
        return NULL;

    switch (event->kind) {
    case EVENT_STATE:
        type = PyUnicode_FromString("state");
        value = PyLong_FromLong(event->state);
        break;
    case EVENT_CANDIDATE:
        type = PyUnicode_FromString("candidate");
        value = PyUnicode_FromString(event->text ? event->text : "");
        break;
    case EVENT_GATHERING_DONE:
        type = PyUnicode_FromString("gatheringDone");
        value = Py_NewRef(Py_None);
        break;
    case EVENT_RECEIVE:
        type = PyUnicode_FromString("receive");
        value = PyBytes_FromStringAndSize((const char *)event->data, (Py_ssize_t)event->size);
        break;
    default:
        Py_DECREF(dictionary);
        PyErr_SetString(PyExc_RuntimeError, "unknown native event");
        return NULL;
    }

    if (!type || !value || PyDict_SetItemString(dictionary, "type", type) < 0 ||
        PyDict_SetItemString(dictionary, "value", value) < 0) {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_DECREF(dictionary);

        return NULL;
    }

    Py_DECREF(type);
    Py_DECREF(value);
    return dictionary;
}

static PyObject *convertReceiveToPython(ReceiveNode *receive) {
    PyObject *dictionary = PyDict_New();
    PyObject *type = NULL;
    PyObject *value = NULL;

    if (!dictionary)
        return NULL;

    type = PyUnicode_FromString("receive");
    value = PyBytes_FromStringAndSize((const char *)receive->data, (Py_ssize_t)receive->size);

    if (!type || !value || PyDict_SetItemString(dictionary, "type", type) < 0 ||
        PyDict_SetItemString(dictionary, "value", value) < 0) {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_DECREF(dictionary);
        return NULL;
    }

    Py_DECREF(type);
    Py_DECREF(value);

    return dictionary;
}

static PyObject *drainEvents(PyP2PAgent *self, Py_ssize_t maxEvents) {
    EventNode *firstEvent = NULL;
    EventNode *lastEvent = NULL;
    ReceiveNode *firstReceive = NULL;
    ReceiveNode *lastReceive = NULL;
    Py_ssize_t count = 0;
    int signalEventAgain = 0;
    int signalReceiveAgain = 0;
    PyObject *events;

    events = PyList_New(0);
    if (!events)
        return NULL;

    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    /* Re-arm the binary any-event wake token while this consumer drains it. */
    if (self->eventSignaled &&
        PyThread_acquire_lock(self->eventAvailable, NOWAIT_LOCK)) {
        self->eventSignaled = 0;
    }

    while (self->head && count < maxEvents) {
        EventNode *event = self->head;
        self->head = event->next;
        if (!self->head)
            self->tail = NULL;

        event->next = NULL;

        if (lastEvent)
            lastEvent->next = event;
        else
            firstEvent = event;

        lastEvent = event;
        ++count;
    }

    while (self->receiveHead && count < maxEvents) {
        ReceiveNode *receive = self->receiveHead;
        self->receiveHead = receive->next;
        if (!self->receiveHead)
            self->receiveTail = NULL;

        receive->next = NULL;

        if (lastReceive)
            lastReceive->next = receive;
        else
            firstReceive = receive;

        lastReceive = receive;
        ++count;
    }

    if (firstReceive && self->receiveSignaled &&
        PyThread_acquire_lock(self->receiveAvailable, NOWAIT_LOCK)) {
        self->receiveSignaled = 0;
    }

    if (self->receiveHead && !self->receiveSignaled) {
        self->receiveSignaled = 1;
        signalReceiveAgain = 1;
    }

    if ((self->head || self->receiveHead) && !self->eventSignaled) {
        self->eventSignaled = 1;
        signalEventAgain = 1;
    }

    PyThread_release_lock(self->lock);

    if (signalReceiveAgain)
        PyThread_release_lock(self->receiveAvailable);

    if (signalEventAgain)
        PyThread_release_lock(self->eventAvailable);

    while (firstEvent) {
        EventNode *event = firstEvent;
        PyObject *pythonEvent;
        firstEvent = event->next;
        event->next = NULL;
        pythonEvent = convertEventToPython(event);
        freeEvent(event);

        if (!pythonEvent || PyList_Append(events, pythonEvent) < 0) {
            Py_XDECREF(pythonEvent);

            while (firstEvent) {
                EventNode *next = firstEvent->next;
                freeEvent(firstEvent);
                firstEvent = next;
            }

            while (firstReceive) {
                ReceiveNode *next = firstReceive->next;
                freeReceive(firstReceive);
                firstReceive = next;
            }

            Py_DECREF(events);
            return NULL;
        }

        Py_DECREF(pythonEvent);
    }

    while (firstReceive) {
        ReceiveNode *receive = firstReceive;
        PyObject *pythonEvent;
        firstReceive = receive->next;
        receive->next = NULL;
        pythonEvent = convertReceiveToPython(receive);
        freeReceive(receive);

        if (!pythonEvent || PyList_Append(events, pythonEvent) < 0) {
            Py_XDECREF(pythonEvent);

            while (firstReceive) {
                ReceiveNode *next = firstReceive->next;
                freeReceive(firstReceive);
                firstReceive = next;
            }

            Py_DECREF(events);
            return NULL;
        }

        Py_DECREF(pythonEvent);
    }
    return events;
}

static PyObject *pollAgentEvents(PyP2PAgent *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t maxEvents = 128;
    static char *keywordList[] = {"maxEvents", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|n", keywordList, &maxEvents))
        return NULL;

    if (maxEvents < 0) {
        PyErr_SetString(PyExc_ValueError, "maxEvents must be >= 0");
        return NULL;
    }

    return drainEvents(self, maxEvents);
}

static PyObject *waitForAgentEvents(PyP2PAgent *self, PyObject *args, PyObject *kwargs) {
    double timeout = 0.0;
    Py_ssize_t maxEvents = 128;
    int eventQueued = 0;
    PyLockStatus waitStatus = PY_LOCK_FAILURE;
    static char *keywordList[] = {"timeout", "maxEvents", NULL};

    if (!requireOpen(self))
        return NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|dn", keywordList, &timeout, &maxEvents))
        return NULL;

    if (timeout < 0.0) {
        PyErr_SetString(PyExc_ValueError, "timeout must be >= 0");
        return NULL;
    }

    if (maxEvents < 0) {
        PyErr_SetString(PyExc_ValueError, "maxEvents must be >= 0");
        return NULL;
    }

    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    eventQueued = self->head != NULL || self->receiveHead != NULL;
    PyThread_release_lock(self->lock);

    if (!eventQueued) {
        PY_TIMEOUT_T timeoutUs = (PY_TIMEOUT_T)(timeout * 1000000.0);
        Py_BEGIN_ALLOW_THREADS
        waitStatus = PyThread_acquire_lock_timed(self->eventAvailable, timeoutUs, 0);
        Py_END_ALLOW_THREADS

        if (waitStatus == PY_LOCK_ACQUIRED) {
            PyThread_acquire_lock(self->lock, WAIT_LOCK);
            self->eventSignaled = 0;
            PyThread_release_lock(self->lock);
        }
    }

    return drainEvents(self, maxEvents);
}

static PyMethodDef agentMethods[] = {
    {"close", (PyCFunction)closeAgent, METH_NOARGS, NULL},
    {"gather", (PyCFunction)gatherAgent, METH_NOARGS, NULL},
    {"holdGathering", (PyCFunction)holdAgentGathering, METH_NOARGS, NULL},
    {"releaseGathering", (PyCFunction)releaseAgentGathering, METH_NOARGS, NULL},
    {"localPort", (PyCFunction)getAgentLocalPort, METH_NOARGS, NULL},
    {"localDescription", (PyCFunction)getAgentLocalDescription, METH_NOARGS, NULL},
    {"setRemoteDescription", (PyCFunction)setAgentRemoteDescription, METH_O, NULL},
    {"addRemoteCandidate", (PyCFunction)addAgentRemoteCandidate, METH_O, NULL},
    {"remoteGatheringDone", (PyCFunction)completeAgentRemoteGathering, METH_NOARGS, NULL},
    {"addMappedCandidate", (PyCFunction)addAgentMappedCandidate, METH_VARARGS, NULL},
    {"send", (PyCFunction)sendAgent, METH_O, NULL},
    {"state", (PyCFunction)getAgentState, METH_NOARGS, NULL},
    {"selectedAddresses", (PyCFunction)getAgentSelectedAddresses, METH_NOARGS, NULL},
    {"pollEvents", FFL_PYC_FUNCTION_CAST(pollAgentEvents), METH_VARARGS | METH_KEYWORDS, NULL},
    {"waitEvents", FFL_PYC_FUNCTION_CAST(waitForAgentEvents), METH_VARARGS | METH_KEYWORDS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyObject *createP2PObject(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    (void)args;
    (void)kwargs;
    return type->tp_alloc(type, 0);
}

static PyType_Slot agentTypeSlots[] = {
    {Py_tp_new, (void *)createP2PObject},
    {Py_tp_init, (void *)initializeAgent},
    {Py_tp_dealloc, (void *)deallocateAgent},
    {Py_tp_methods, (void *)agentMethods},
    {0, NULL}
};

static PyType_Spec agentTypeSpec = {
    "_ffl_p2p.Agent",
    sizeof(PyP2PAgent),
    0,
    Py_TPFLAGS_DEFAULT,
    agentTypeSlots
};

typedef struct PyP2PPortMapping {
    PyObject_HEAD
    FFLP2PPortMapping *mapping;
    int closed;
} PyP2PPortMapping;

static int initializePortMapping(PyP2PPortMapping *self, PyObject *args, PyObject *kwargs) {
    const char *protocol;
    unsigned int internalPort;
    plum_ip_protocol_t nativeProtocol;
    static char *keywordList[] = {"protocol", "internalPort", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sI", keywordList, &protocol, &internalPort))
        return -1;

    if (!internalPort || internalPort > 65535) {
        PyErr_SetString(PyExc_ValueError, "internalPort out of range");
        return -1;
    }

    if (strcmp(protocol, "udp") == 0)
        nativeProtocol = PLUM_IP_PROTOCOL_UDP;
    else if (strcmp(protocol, "tcp") == 0)
        nativeProtocol = PLUM_IP_PROTOCOL_TCP;
    else {
        PyErr_SetString(PyExc_ValueError, "protocol must be 'udp' or 'tcp'");
        return -1;
    }

    self->mapping = createFFLP2PPortMapping(nativeProtocol, (uint16_t)internalPort);
    if (!self->mapping) {
        PyErr_SetString(PyExc_RuntimeError, "port mapping creation failed");
        return -1;
    }

    return 0;
}

static void deallocatePortMapping(PyP2PPortMapping *self) {
    if (self->mapping) {
        destroyFFLP2PPortMapping(self->mapping);
        self->mapping = NULL;
    }

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *closePortMapping(PyP2PPortMapping *self, PyObject *Py_UNUSED(ignored)) {
    if (self->mapping) {
        destroyFFLP2PPortMapping(self->mapping);
        self->mapping = NULL;
    }
    self->closed = 1;

    Py_RETURN_NONE;
}

static const char *getMappingStateName(plum_state_t state) {
    switch (state) {
    case PLUM_STATE_DESTROYED:
        return "destroyed";

    case PLUM_STATE_PENDING:
        return "pending";

    case PLUM_STATE_SUCCESS:
        return "success";

    case PLUM_STATE_FAILURE:
        return "failure";

    case PLUM_STATE_DESTROYING:
        return "destroying";

    default:
        return "unknown";
    }
}

static const char *getMappingProtocolName(plum_mapping_protocol_t protocol) {
    switch (protocol) {
    case PLUM_MAPPING_PROTOCOL_PCP:
        return "pcp";

    case PLUM_MAPPING_PROTOCOL_NATPMP:
        return "nat-pmp";

    case PLUM_MAPPING_PROTOCOL_UPNP:
        return "upnp";

    case PLUM_MAPPING_PROTOCOL_DIRECT:
        return "direct";

    default:
        return "unknown";
    }
}

static PyObject *queryPortMapping(PyP2PPortMapping *self, PyObject *Py_UNUSED(ignored)) {
    FFLP2PPortMappingResult result;
    PyObject *dictionary;
    int queryResult;

    if (!self->mapping || self->closed) {
        PyErr_SetString(PyExc_RuntimeError, "mapping is closed");
        return NULL;
    }

    queryResult = queryFFLP2PPortMapping(self->mapping, &result);
    if (queryResult != PLUM_ERR_SUCCESS) {
        PyErr_Format(PyExc_RuntimeError, "port mapping query failed: libplum error %d", queryResult);
        return NULL;
    }

    dictionary = PyDict_New();
    if (!dictionary)
        return NULL;

    if (setDictionaryString(dictionary, "state", getMappingStateName(result.state)) < 0 ||
        setDictionaryString(dictionary, "mappingProtocol", getMappingProtocolName(result.mappingProtocol)) < 0 ||
        setDictionaryUInt64(dictionary, "internalPort", result.internalPort) < 0 ||
        setDictionaryUInt64(dictionary, "externalPort", result.externalPort) < 0 ||
        setDictionaryString(dictionary, "externalHost", result.externalHost) < 0) {
        Py_DECREF(dictionary);
        return NULL;
    }

    return dictionary;
}

static PyMethodDef portMappingMethods[] = {
    {"query", (PyCFunction)queryPortMapping, METH_NOARGS, NULL},
    {"close", (PyCFunction)closePortMapping, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot portMappingTypeSlots[] = {
    {Py_tp_new, (void *)createP2PObject},
    {Py_tp_init, (void *)initializePortMapping},
    {Py_tp_dealloc, (void *)deallocatePortMapping},
    {Py_tp_methods, (void *)portMappingMethods},
    {0, NULL}
};

static PyType_Spec portMappingTypeSpec = {
    "_ffl_p2p.PortMapping",
    sizeof(PyP2PPortMapping),
    0,
    Py_TPFLAGS_DEFAULT,
    portMappingTypeSlots
};

static PyMethodDef moduleMethods[] = {
    {"setLogLevel", (PyCFunction)setLogLevel, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef moduleDefinition = {
    PyModuleDef_HEAD_INIT,
    "_ffl_p2p",
    "Native libjuice/libplum primitives for ffl-p2p",
    -1,
    moduleMethods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__ffl_p2p(void) {
    PyObject *agentType = PyType_FromSpec(&agentTypeSpec);
    PyObject *portMappingType;
    PyObject *module;

    if (!agentType)
        return NULL;

    gAgentType = (PyTypeObject *)agentType;

    portMappingType = PyType_FromSpec(&portMappingTypeSpec);
    if (!portMappingType) {
        Py_DECREF(agentType);
        return NULL;
    }

    module = PyModule_Create(&moduleDefinition);
    if (!module) {
        Py_DECREF(agentType);
        Py_DECREF(portMappingType);
        return NULL;
    }

    if (PyModule_AddObject(module, "Agent", agentType) < 0) {
        Py_DECREF(agentType);
        Py_DECREF(portMappingType);
        Py_DECREF(module);
        return NULL;
    }

    if (PyModule_AddObject(module, "PortMapping", portMappingType) < 0) {
        Py_DECREF(portMappingType);
        Py_DECREF(module);
        return NULL;
    }

    PyModule_AddIntConstant(module, "JUICE_STATE_DISCONNECTED", JUICE_STATE_DISCONNECTED);
    PyModule_AddIntConstant(module, "JUICE_STATE_GATHERING", JUICE_STATE_GATHERING);
    PyModule_AddIntConstant(module, "JUICE_STATE_CONNECTING", JUICE_STATE_CONNECTING);
    PyModule_AddIntConstant(module, "JUICE_STATE_CONNECTED", JUICE_STATE_CONNECTED);
    PyModule_AddIntConstant(module, "JUICE_STATE_COMPLETED", JUICE_STATE_COMPLETED);
    PyModule_AddIntConstant(module, "JUICE_STATE_FAILED", JUICE_STATE_FAILED);
    PyModule_AddStringConstant(module, "FFL_P2P_NATIVE_BUILD",
                               "0.2.5");
#ifdef FFL_P2P_HAVE_QUIC
    if (registerFFLP2PQUIC(module) < 0) {
        Py_DECREF(module);
        return NULL;
    }
#endif

    return module;
}
