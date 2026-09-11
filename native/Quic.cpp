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

#include "Quic.h"
#include "Agent.h"
#include "quic/Connection.h"
#include "quic/Credentials.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _PyCFunction_CAST
#define FFL_QUIC_PYC_FUNCTION_CAST(function) _PyCFunction_CAST(function)
#else
#define FFL_QUIC_PYC_FUNCTION_CAST(function) (PyCFunction)(void (*)(void))(function)
#endif

namespace {

constexpr char kALPN[] = "ffl-p2p/1";

struct PyQUICCredentials {
    PyObject_HEAD
    ffl::quic::Credentials *state;
};

struct PyQUICSession {
    PyObject_HEAD
    std::shared_ptr<ffl::quic::Connection> *connection;
    PyObject *agentObject;
    std::vector<uint8_t> *initialPacket;
    int started;
};

PyTypeObject *gCredentialsType = nullptr;

PyObject *createObject(PyTypeObject *type, PyObject *, PyObject *) {
    return type->tp_alloc(type, 0);
}

int initializeCredentials(PyQUICCredentials *self, PyObject *, PyObject *) {
    try {
        self->state = ffl::quic::generateCredentials().release();
        return 0;
    } catch (const std::exception &error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return -1;
    }
}

void deallocateCredentials(PyQUICCredentials *self) {
    delete self->state;
    self->state = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

PyObject *getCredentialsCertificate(PyQUICCredentials *self, void *) {
    if (!self->state) {
        PyErr_SetString(PyExc_RuntimeError, "QUIC credentials are closed");
        return nullptr;
    }

    return PyUnicode_DecodeUTF8(
        self->state->certificatePEM.data(),
        static_cast<Py_ssize_t>(self->state->certificatePEM.size()), "strict");
}

PyGetSetDef credentialsGetSet[] = {
    {const_cast<char *>("certificate"),
     reinterpret_cast<getter>(getCredentialsCertificate), nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

PyType_Slot credentialsTypeSlots[] = {
    {Py_tp_new, reinterpret_cast<void *>(createObject)},
    {Py_tp_init, reinterpret_cast<void *>(initializeCredentials)},
    {Py_tp_dealloc, reinterpret_cast<void *>(deallocateCredentials)},
    {Py_tp_getset, reinterpret_cast<void *>(credentialsGetSet)},
    {0, nullptr}
};

PyType_Spec credentialsTypeSpec = {
    "_ffl_p2p.QUICCredentials",
    sizeof(PyQUICCredentials),
    0,
    Py_TPFLAGS_DEFAULT,
    credentialsTypeSlots
};

bool requireConnection(PyQUICSession *self) {
    if (!self->connection || !*self->connection) {
        PyErr_SetString(PyExc_RuntimeError, "QUIC session is closed");
        return false;
    }

    const std::string error = (*self->connection)->getError();
    if (!error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error.c_str());
        return false;
    }

    return true;
}

int initializeSession(PyQUICSession *self, PyObject *args, PyObject *kwargs) {
    const char *role = nullptr;
    const char *certificate = nullptr;
    Py_ssize_t certificateLength = 0;
    PyObject *credentialsObject = Py_None;
    Py_buffer initialPacket{};
    static char *keywordList[] = {
        const_cast<char *>("role"),
        const_cast<char *>("certificate"),
        const_cast<char *>("credentials"),
        const_cast<char *>("initialPacket"),
        nullptr
    };

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "s|z#Oy*", keywordList,
            &role, &certificate, &certificateLength,
            &credentialsObject, &initialPacket))
        return -1;

    try {
        std::shared_ptr<ffl::quic::Connection> connection;

        if (std::strcmp(role, "client") == 0) {
            if (!certificate || certificateLength <= 0)
                throw std::runtime_error("client requires the pinned QUIC certificate");

            connection = ffl::quic::Connection::createClient(
                std::string(certificate, static_cast<size_t>(certificateLength)));
        } else if (std::strcmp(role, "server") == 0) {
            if (!gCredentialsType || !PyObject_TypeCheck(credentialsObject, gCredentialsType))
                throw std::runtime_error("server requires QUICCredentials");

            auto *credentials = reinterpret_cast<PyQUICCredentials *>(credentialsObject);
            if (!credentials->state)
                throw std::runtime_error("QUIC credentials are closed");

            connection = ffl::quic::Connection::createServer(*credentials->state);
        } else {
            throw std::runtime_error("role must be 'client' or 'server'");
        }

        self->connection = new std::shared_ptr<ffl::quic::Connection>(std::move(connection));
        self->agentObject = nullptr;
        self->initialPacket = nullptr;
        self->started = 0;

        if (initialPacket.buf && initialPacket.len > 0) {
            const auto *bytes = static_cast<const uint8_t *>(initialPacket.buf);
            self->initialPacket = new std::vector<uint8_t>(
                bytes, bytes + static_cast<size_t>(initialPacket.len));
        }

        PyBuffer_Release(&initialPacket);
        return 0;
    } catch (const std::exception &error) {
        PyBuffer_Release(&initialPacket);
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return -1;
    }
}

void closeSessionNoThrow(PyQUICSession *self) {
    if (!self->connection || !*self->connection)
        return;
    try {
        (*self->connection)->close(1.0);
    } catch (...) {
    }
}

void deallocateSession(PyQUICSession *self) {
    if (self->connection) {
        Py_BEGIN_ALLOW_THREADS
        closeSessionNoThrow(self);
        Py_END_ALLOW_THREADS
        delete self->connection;
        self->connection = nullptr;
    }

    delete self->initialPacket;
    self->initialPacket = nullptr;

    Py_XDECREF(self->agentObject);

    self->agentObject = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

PyObject *startSession(PyQUICSession *self, PyObject *args, PyObject *kwargs) {
    if (!requireConnection(self))
        return nullptr;

    PyObject *agentObject = nullptr;
    int aggregate = 1;
    double timeout = 5.0;
    static char *keywordList[] = {
        const_cast<char *>("agent"),
        const_cast<char *>("aggregate"),
        const_cast<char *>("timeout"),
        nullptr
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|pd", keywordList,
                                     &agentObject, &aggregate, &timeout))
        return nullptr;

    if (timeout <= 0.0) {
        PyErr_SetString(PyExc_ValueError, "timeout must be positive");
        return nullptr;
    }

    if (self->started) {
        PyErr_SetString(PyExc_RuntimeError, "QUIC runtime is already started");
        return nullptr;
    }

    void *agentHandle = getFFLP2PAgentNativeHandle(agentObject);
    if (!agentHandle)
        return nullptr;

    Py_INCREF(agentObject);
    std::string errorMessage;

    Py_BEGIN_ALLOW_THREADS
    try {
        (*self->connection)->start(agentHandle, aggregate != 0, timeout);
        if (self->initialPacket && !self->initialPacket->empty()) {
            (*self->connection)->queueReceive(
                self->initialPacket->data(), self->initialPacket->size());
        }
    } catch (const std::exception &error) {
        errorMessage = error.what();
    }
    Py_END_ALLOW_THREADS

    if (!errorMessage.empty()) {
        Py_DECREF(agentObject);
        PyErr_SetString(PyExc_RuntimeError, errorMessage.c_str());
        return nullptr;
    }

    self->agentObject = agentObject;
    self->started = 1;

    delete self->initialPacket;
    self->initialPacket = nullptr;

    Py_RETURN_NONE;
}

PyObject *queueSessionAsync(PyQUICSession *self, PyObject *args, PyObject *kwargs) {
    if (!requireConnection(self))
        return nullptr;

    Py_buffer data{};
    int fin = 0;
    static char *keywordList[] = {
        const_cast<char *>("data"),
        const_cast<char *>("fin"),
        nullptr
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*|p", keywordList, &data, &fin))
        return nullptr;

    std::string errorMessage;

    Py_BEGIN_ALLOW_THREADS
    try {
        (*self->connection)->queueDataAsync(
            data.buf, static_cast<size_t>(data.len), fin != 0);
    } catch (const std::exception &error) {
        errorMessage = error.what();
    }
    Py_END_ALLOW_THREADS

    PyBuffer_Release(&data);

    if (!errorMessage.empty()) {
        PyErr_SetString(PyExc_RuntimeError, errorMessage.c_str());
        return nullptr;
    }

    Py_RETURN_NONE;
}

PyObject *waitForSession(PyQUICSession *self, PyObject *args, PyObject *kwargs) {
    if (!requireConnection(self))
        return nullptr;

    double timeout = 0.01;
    static char *keywordList[] = {const_cast<char *>("timeout"), nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|d", keywordList, &timeout))
        return nullptr;

    if (timeout < 0.0) {
        PyErr_SetString(PyExc_ValueError, "timeout must be >= 0");
        return nullptr;
    }

    bool changed = false;
    Py_BEGIN_ALLOW_THREADS
    changed = (*self->connection)->waitForChange(timeout);
    Py_END_ALLOW_THREADS

    return PyBool_FromLong(changed ? 1 : 0);
}

PyObject *checkSessionError(PyQUICSession *self, PyObject *Py_UNUSED(ignored)) {
    if (!requireConnection(self))
        return nullptr;

    const std::string errorMessage = (*self->connection)->getError();
    if (!errorMessage.empty()) {
        PyErr_SetString(PyExc_RuntimeError, errorMessage.c_str());
        return nullptr;
    }

    Py_RETURN_NONE;
}

PyObject *readSession(PyQUICSession *self, PyObject *Py_UNUSED(ignored)) {
    if (!requireConnection(self))
        return nullptr;

    std::vector<uint8_t> data;
    std::string errorMessage;

    Py_BEGIN_ALLOW_THREADS
    try {
        data = (*self->connection)->read();
    } catch (const std::exception &error) {
        errorMessage = error.what();
    }
    Py_END_ALLOW_THREADS

    if (!errorMessage.empty()) {
        PyErr_SetString(PyExc_RuntimeError, errorMessage.c_str());
        return nullptr;
    }

    return PyBytes_FromStringAndSize(
        data.empty() ? "" : reinterpret_cast<const char *>(data.data()),
        static_cast<Py_ssize_t>(data.size()));
}

PyObject *closeSession(PyQUICSession *self, PyObject *Py_UNUSED(ignored)) {
    if (!self->connection || !*self->connection)
        Py_RETURN_NONE;

    std::string errorMessage;

    Py_BEGIN_ALLOW_THREADS
    try {
        (*self->connection)->close(5.0);
    } catch (const std::exception &error) {
        errorMessage = error.what();
    }
    Py_END_ALLOW_THREADS

    Py_CLEAR(self->agentObject);
    self->started = 0;
    if (!errorMessage.empty()) {
        PyErr_SetString(PyExc_RuntimeError, errorMessage.c_str());
        return nullptr;
    }

    Py_RETURN_NONE;
}

PyObject *getSessionHandshakeComplete(PyQUICSession *self, void *) {
    if (!requireConnection(self))
        return nullptr;

    return PyBool_FromLong((*self->connection)->isHandshakeComplete() ? 1 : 0);
}

PyObject *getSessionHandshakeConfirmed(PyQUICSession *self, void *) {
    if (!requireConnection(self))
        return nullptr;

    return PyBool_FromLong((*self->connection)->isHandshakeConfirmed() ? 1 : 0);
}

PyObject *getSessionPeerFinished(PyQUICSession *self, void *) {
    if (!requireConnection(self))
        return nullptr;

    return PyBool_FromLong((*self->connection)->isPeerFinished() ? 1 : 0);
}

PyObject *getSessionPendingWrite(PyQUICSession *self, void *) {
    if (!requireConnection(self))
        return nullptr;

    return PyBool_FromLong((*self->connection)->hasPendingWrite() ? 1 : 0);
}

PyObject *getSessionBufferedWriteBytes(PyQUICSession *self, void *) {
    if (!requireConnection(self))
        return nullptr;

    return PyLong_FromUnsignedLongLong((*self->connection)->getBufferedWriteBytes());
}

PyObject *getSessionWriteAcknowledged(PyQUICSession *self, void *) {
    if (!requireConnection(self))
        return nullptr;

    return PyBool_FromLong((*self->connection)->isWriteAcknowledged() ? 1 : 0);
}

PyObject *getSessionStreamClosed(PyQUICSession *self, void *) {
    if (!requireConnection(self))
        return nullptr;

    return PyBool_FromLong((*self->connection)->isStreamClosed() ? 1 : 0);
}

PyObject *getSessionRuntimeV2(PyQUICSession *, void *) {
    Py_RETURN_TRUE;
}

PyMethodDef sessionMethods[] = {
    {"start", FFL_QUIC_PYC_FUNCTION_CAST(startSession), METH_VARARGS | METH_KEYWORDS, nullptr},
    {"queueAsync", FFL_QUIC_PYC_FUNCTION_CAST(queueSessionAsync), METH_VARARGS | METH_KEYWORDS, nullptr},
    {"wait", FFL_QUIC_PYC_FUNCTION_CAST(waitForSession), METH_VARARGS | METH_KEYWORDS, nullptr},
    {"checkError", reinterpret_cast<PyCFunction>(checkSessionError), METH_NOARGS, nullptr},
    {"read", reinterpret_cast<PyCFunction>(readSession), METH_NOARGS, nullptr},
    {"close", reinterpret_cast<PyCFunction>(closeSession), METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};

PyGetSetDef sessionGetSet[] = {
    {const_cast<char *>("handshakeComplete"), reinterpret_cast<getter>(getSessionHandshakeComplete), nullptr, nullptr, nullptr},
    {const_cast<char *>("handshakeConfirmed"), reinterpret_cast<getter>(getSessionHandshakeConfirmed), nullptr, nullptr, nullptr},
    {const_cast<char *>("peerFinished"), reinterpret_cast<getter>(getSessionPeerFinished), nullptr, nullptr, nullptr},
    {const_cast<char *>("pendingWrite"), reinterpret_cast<getter>(getSessionPendingWrite), nullptr, nullptr, nullptr},
    {const_cast<char *>("bufferedWriteBytes"), reinterpret_cast<getter>(getSessionBufferedWriteBytes), nullptr, nullptr, nullptr},
    {const_cast<char *>("writeAcknowledged"), reinterpret_cast<getter>(getSessionWriteAcknowledged), nullptr, nullptr, nullptr},
    {const_cast<char *>("streamClosed"), reinterpret_cast<getter>(getSessionStreamClosed), nullptr, nullptr, nullptr},
    {const_cast<char *>("runtimeV2"), reinterpret_cast<getter>(getSessionRuntimeV2), nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

PyType_Slot sessionTypeSlots[] = {
    {Py_tp_new, reinterpret_cast<void *>(createObject)},
    {Py_tp_init, reinterpret_cast<void *>(initializeSession)},
    {Py_tp_dealloc, reinterpret_cast<void *>(deallocateSession)},
    {Py_tp_methods, reinterpret_cast<void *>(sessionMethods)},
    {Py_tp_getset, reinterpret_cast<void *>(sessionGetSet)},
    {0, nullptr}
};

PyType_Spec sessionTypeSpec = {
    "_ffl_p2p.QUICSession",
    sizeof(PyQUICSession),
    0,
    Py_TPFLAGS_DEFAULT,
    sessionTypeSlots
};

} // namespace

extern "C" int registerFFLP2PQUIC(PyObject *module) {
    PyObject *credentialsType = PyType_FromSpec(&credentialsTypeSpec);
    if (!credentialsType)
        return -1;

    gCredentialsType = reinterpret_cast<PyTypeObject *>(credentialsType);
    if (PyModule_AddObject(module, "QUICCredentials", credentialsType) < 0) {
        Py_DECREF(credentialsType);
        gCredentialsType = nullptr;
        return -1;
    }

    PyObject *sessionType = PyType_FromSpec(&sessionTypeSpec);
    if (!sessionType)
        return -1;

    if (PyModule_AddObject(module, "QUICSession", sessionType) < 0) {
        Py_DECREF(sessionType);
        return -1;
    }

    if (PyModule_AddStringConstant(module, "QUIC_ALPN", kALPN) < 0)
        return -1;

    return 0;
}
