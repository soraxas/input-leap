/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "net/TCPListenSocket.h"

#include "net/NetworkAddress.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"
#include "net/TSocketMultiplexerMethodJob.h"
#include "net/XSocket.h"
#include "io/XIO.h"
#include "arch/Arch.h"
#include "arch/XArch.h"
#include "base/IEventQueue.h"

//
// TCPListenSocket
//

TCPListenSocket::TCPListenSocket(IEventQueue* events, SocketMultiplexer* socketMultiplexer, IArchNetwork::EAddressFamily family) :
    m_events(events),
    m_socketMultiplexer(socketMultiplexer)
{
    try {
        m_socket = ARCH->newSocket(family, IArchNetwork::kSTREAM);
    }
    catch (XArchNetwork& e) {
        throw XSocketCreate(e.what());
    }
}

TCPListenSocket::~TCPListenSocket()
{
    try {
        if (m_socket != NULL) {
            m_socketMultiplexer->removeSocket(this);
            ARCH->closeSocket(m_socket);
        }
    }
    catch (...) {
        // ignore
    }
}

void
TCPListenSocket::bind(const NetworkAddress& addr)
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ARCH->setReuseAddrOnSocket(m_socket, true);
        ARCH->bindSocket(m_socket, addr.getAddress());
        ARCH->listenOnSocket(m_socket);

        auto new_job = std::make_unique<TSocketMultiplexerMethodJob>(
                    [this](auto j, auto r, auto w, auto e)
                    { return serviceListening(j, r, w, e); },
                    m_socket, true, false);

        m_socketMultiplexer->addSocket(this, std::move(new_job));
    }
    catch (XArchNetworkAddressInUse& e) {
        throw XSocketAddressInUse(e.what());
    }
    catch (XArchNetwork& e) {
        throw XSocketBind(e.what());
    }
}

void
TCPListenSocket::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (m_socket == NULL) {
        throw XIOClosed();
    }
    try {
        m_socketMultiplexer->removeSocket(this);
        ARCH->closeSocket(m_socket);
        m_socket = NULL;
    }
    catch (XArchNetwork& e) {
        throw XSocketIOClose(e.what());
    }
}

void*
TCPListenSocket::getEventTarget() const
{
    return const_cast<void*>(static_cast<const void*>(this));
}

IDataSocket*
TCPListenSocket::accept()
{
    IDataSocket* socket = NULL;
    try {
        socket = new TCPSocket(m_events, m_socketMultiplexer, ARCH->acceptSocket(m_socket, NULL));
        if (socket != NULL) {
            setListeningJob();
        }
        return socket;
    }
    catch (XArchNetwork&) {
        if (socket != NULL) {
            delete socket;
            setListeningJob();
        }
        return NULL;
    }
    catch (std::exception &ex) {
        if (socket != NULL) {
            delete socket;
            setListeningJob();
        }
        throw ex;
    }
}

void
TCPListenSocket::setListeningJob()
{
    auto new_job = std::make_unique<TSocketMultiplexerMethodJob>(
                [this](auto j, auto r, auto w, auto e)
                { return serviceListening(j, r, w, e); },
                m_socket, true, false);
    m_socketMultiplexer->addSocket(this, std::move(new_job));
}

MultiplexerJobStatus TCPListenSocket::serviceListening(ISocketMultiplexerJob* job,
                                                       bool read, bool, bool error)
{
    if (error) {
        close();
        return {false, {}};
    }
    if (read) {
        m_events->addEvent(Event(m_events->forIListenSocket().connecting(), this, NULL));
        // stop polling on this socket until the client accepts
        return {false, {}};
    }
    return {true, {}};
}
