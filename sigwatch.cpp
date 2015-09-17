/*
 * Unix signal watcher for Qt.
 *
 * Copyright (C) 2014 Simon Knopp
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "sigwatch.h"
#include <QDebug>

#ifdef Q_OS_WIN
#include <QMutex>
#endif // Q_OS_WIN

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <QSocketNotifier>
#endif


/*!
 * \brief The UnixSignalWatcherPrivate class implements the back-end signal
 * handling for the UnixSignalWatcher.
 *
 * \see http://qt-project.org/doc/qt-5.0/qtdoc/unix-signals.html
 */
class UnixSignalWatcherPrivate
{
    UnixSignalWatcher * const q_ptr;
    Q_DECLARE_PUBLIC(UnixSignalWatcher)

public:
    UnixSignalWatcherPrivate(UnixSignalWatcher *q);
    ~UnixSignalWatcherPrivate();

    void watchForSignal(int signal);
    static void signalHandler(int signal);

    void emitQtSignal(int signal);

    const char* signalToString(int signal) const;

    void _q_onNotify(int sockfd);

private:
    QList<int> watchedSignals;

#ifdef Q_OS_WIN
    static QMutex instanceGuard;
    static UnixSignalWatcherPrivate *instance;
#endif

#ifdef Q_OS_UNIX
    static int sockpair[2];
    QSocketNotifier *notifier;
#endif
};

#ifdef Q_OS_UNIX
int UnixSignalWatcherPrivate::sockpair[2];
#endif

#ifdef Q_OS_WIN
QMutex UnixSignalWatcherPrivate::instanceGuard;
UnixSignalWatcherPrivate* UnixSignalWatcherPrivate::instance;
#endif


UnixSignalWatcherPrivate::UnixSignalWatcherPrivate(UnixSignalWatcher *q) :
    q_ptr(q)
{
#if defined(Q_OS_UNIX)

    // Create socket pair
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockpair)) {
        qDebug() << "UnixSignalWatcher: socketpair: " << ::strerror(errno);
        return;
    }

    // Create a notifier for the read end of the pair
    notifier = new QSocketNotifier(sockpair[1], QSocketNotifier::Read);
    QObject::connect(notifier, SIGNAL(activated(int)), q, SLOT(_q_onNotify(int)));
    notifier->setEnabled(true);
#elif defined(Q_OS_WIN)

    QMutexLocker lck(&instanceGuard);
    Q_ASSERT_X(!instance, "UnixSignalWatcher", "Cannot create more than one instance of UnixSignalWatcher");
    instance = this;
#else
#   error "UnixSignalWatcher is not supported on this system"
#endif
}

UnixSignalWatcherPrivate::~UnixSignalWatcherPrivate()
{
#if defined(Q_OS_UNIX)
    delete notifier;
#elif defined(Q_OS_WIN)
    QMutexLocker lck(&instanceGuard);
    instance = 0;
#endif
}

/*!
 * Registers a handler for the given Unix \a signal. The handler will write to
 * a socket pair, the other end of which is connected to a QSocketNotifier.
 * This provides a way to break out of the asynchronous context from which the
 * signal handler is called and back into the Qt event loop.
 */
void UnixSignalWatcherPrivate::watchForSignal(int signal)
{
    if (watchedSignals.contains(signal)) {
        qDebug() << "Already watching for signal" << signal;
        return;
    }

#if defined(Q_OS_UNIX)
    // Register a sigaction which will write to the socket pair
    struct sigaction sigact;
    sigact.sa_handler = UnixSignalWatcherPrivate::signalHandler;
    ::sigemptyset(&sigact.sa_mask);
    sigact.sa_flags |= SA_RESTART;
    if (::sigaction(signal, &sigact, NULL)) {
        qDebug() << "UnixSignalWatcher: sigaction: " << ::strerror(errno);
        return;
    }

#elif defined(Q_OS_WIN)
    // Register signal handler.
    if (::signal(signal, UnixSignalWatcherPrivate::signalHandler) == SIG_ERR) {
        qDebug() << "UnixSignalWatcher: signal: " << ::strerror(errno);
        return;
    }

#else
#   error "UnixSignalWatcher is not supported on this system"
#endif

    watchedSignals.append(signal);
}

/*!
 * Called when a Unix \a signal is received. Write to the socket to wake up the
 * QSocketNotifier. On Windows this calls the UnixSignalWatcher::unixSignal(int)
 * Qt signal directly.
 */
void UnixSignalWatcherPrivate::signalHandler(int signal)
{
#if defined(Q_OS_UNIX)
    ssize_t nBytes = ::write(sockpair[0], &signal, sizeof(signal));
    Q_UNUSED(nBytes);

#elif defined(Q_OS_WIN)
    QMutexLocker lck(&instanceGuard);
    //NOTE: Because Windows creates a special thread through which it delivers
    //      Unix signals, there is no danger of deadlock, even if another thread
    //      of the program was inside destructor's mutex lock at the time this
    //      signal was delivered. In that case, the signal will just be ignored
    //      because instance will be atomically set to zero and
    //      UnixSignalWatcher destroyed.
    if (instance) {
        instance->emitQtSignal(signal);
    }

#else
#   error "UnixSignalWatcher is not supported on this system"
#endif
}

/*!
 * Emits the Qt signal(s) on UnixSignalWatcher public interface for the given
 * Unix \a signal.
 */
void UnixSignalWatcherPrivate::emitQtSignal(int signal)
{
    Q_Q(UnixSignalWatcher);

    qDebug() << "Caught signal:" << signalToString(signal);

    emit q->unixSignal(signal);
    if (signal == SIGINT) emit q->interrupted();
    if (signal == SIGTERM) emit q->terminated();
#ifdef Q_OS_UNIX
    if (signal == SIGHUP) emit q->hungup();
#endif
#ifdef Q_OS_WIN
    if (signal == SIGBREAK) emit q->broken();
#endif
}

const char *UnixSignalWatcherPrivate::signalToString(int signal) const
{
#if defined(Q_OS_UNIX)
    return ::strsignal(signal);

#elif defined(Q_OS_WIN)
    switch (signal) {
    case SIGINT:
        return "Interrupt";
    case SIGILL:
        return "Illegal instruction";
    case SIGFPE:
        return "Arithmetic exception";
    case SIGSEGV:
        return "Segmentation fault";
    case SIGTERM:
        return "Terminated";
    case SIGBREAK:
        return "Break";
    case SIGABRT:
    case SIGABRT_COMPAT:
        return "Aborted";
    default:
        return "Other signal";
    }
#endif
}

/*!
 * Called when the signal handler has written to the socket pair. Emits the Unix
 * signal as a Qt signal.
 */
void UnixSignalWatcherPrivate::_q_onNotify(int sockfd)
{
#ifdef Q_OS_UNIX
    int signal;
    ssize_t nBytes = ::read(sockfd, &signal, sizeof(signal));
    Q_UNUSED(nBytes);

    emitQtSignal(signal);

#else
    Q_UNUSED(sockfd)
#endif // Q_OS_UNIX
}


/*!
 * Create a new UnixSignalWatcher as a child of the given \a parent.
 */
UnixSignalWatcher::UnixSignalWatcher(QObject *parent) :
    QObject(parent),
    d_ptr(new UnixSignalWatcherPrivate(this))
{
}

/*!
 * Destroy this UnixSignalWatcher.
 */
UnixSignalWatcher::~UnixSignalWatcher()
{
    delete d_ptr;
}

/*!
 * Register a signal handler for the given \a signal.
 *
 * After calling this method you can \c connect() to the unixSignal() Qt signal
 * to be notified when the Unix signal is received.
 */
void UnixSignalWatcher::watchForSignal(int signal)
{
    Q_D(UnixSignalWatcher);
    d->watchForSignal(signal);
}

void UnixSignalWatcher::watchForInterrupt()
{
    watchForSignal(SIGINT);
}

void UnixSignalWatcher::watchForTerminate()
{
    watchForSignal(SIGTERM);
}

void UnixSignalWatcher::watchForHangup()
{
#ifdef Q_OS_UNIX
    watchForSignal(SIGHUP);
#else
    Q_ASSERT_X(false, "UnixSignalWatcher", "SIGHUP is not supported on this system");
#endif
}

void UnixSignalWatcher::watchForBreak()
{
#ifdef Q_OS_WIN
    watchForSignal(SIGBREAK);
#else
    Q_ASSERT_X(false, "UnixSignalWatcher", "SIGBREAK is not supported on this system");
#endif
}

/*!
 * \fn void UnixSignalWatcher::unixSignal(int signal)
 * Emitted when the given Unix \a signal is received.
 *
 * watchForSignal() must be called for each Unix signal that you want to receive
 * via the unixSignal() Qt signal. If a watcher is watching multiple signals,
 * unixSignal() will be emitted whenever *any* of the watched Unix signals are
 * received, and the \a signal argument can be inspected to find out which one
 * was actually received.
 *
 * In addition, the corresponding convenience Qt signals interrupted(),
 * terminated() and hangup() are emitted when the caught \a signal is SIGINT,
 * SIGTERM and SIGHUP respectively.
 */

#include "moc_sigwatch.cpp"
