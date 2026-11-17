/******************************************************************************
 * testprocessshutdown.cpp
 * tbc-analyse - unit tests for ProcessShutdown::stopForShutdown
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

// Tests must assert even in Release builds, where NDEBUG would otherwise
// compile every assert() away
#undef NDEBUG
#include <cassert>
#include <iostream>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QStringList>

#include <csignal>
#include <cerrno>
#include <sys/types.h>

#include "processshutdown.h"

namespace {

// Stands in for ExportDialog: counts every signal its process delivers. A
// delivery during shutdown is what crashed tbc-analyse (its handler touched
// a UI that was already deleted).
struct Owner : QObject {
    int deliveries = 0;

    void watch(QProcess *process)
    {
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { ++deliveries; });
        connect(process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { ++deliveries; });
        connect(process, &QProcess::readyReadStandardError, this, [this]() { ++deliveries; });
    }
};

bool pidIsGone(qint64 pid)
{
    return ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

void testARunningProcessIsStoppedWithoutReachingItsOwner()
{
    Owner owner;
    auto *process = new QProcess(&owner);
    owner.watch(process);
    process->start(QStringLiteral("sleep"), {QStringLiteral("30")});
    assert(process->waitForStarted(5000));
    const qint64 pid = process->processId();

    ProcessShutdown::stopForShutdown(&owner, {process});

    assert(process->state() == QProcess::NotRunning);
    assert(pidIsGone(pid));
    assert(owner.deliveries == 0);
    std::cerr << "PASS: a running process is stopped without reaching its owner\n";
}

void testAProcessThatIgnoresSigintIsKilledAfterTheGrace()
{
    Owner owner;
    auto *process = new QProcess(&owner);
    owner.watch(process);
    // Says "ready" only once SIGINT is ignored (signalling sooner races the
    // trap), then execs sleep, which inherits the ignored SIGINT.
    process->start(QStringLiteral("sh"),
                   {QStringLiteral("-c"), QStringLiteral("trap '' INT; echo ready; exec sleep 30")});
    assert(process->waitForStarted(5000));
    assert(process->waitForReadyRead(5000));
    assert(process->readAllStandardOutput().startsWith("ready"));
    const qint64 pid = process->processId();

    QElapsedTimer clock;
    clock.start();
    ProcessShutdown::stopForShutdown(&owner, {process}, 300);

    assert(process->state() == QProcess::NotRunning);
    assert(pidIsGone(pid));
    assert(clock.elapsed() >= 300);   // SIGINT was really ignored: the grace ran out
    assert(clock.elapsed() < 5000);   // then kill(); never the child's 30 s
    assert(owner.deliveries == 0);
    std::cerr << "PASS: a process ignoring SIGINT is killed after the grace\n";
}

void testDestroyingTheOwnerAfterShutdownDeliversNothing()
{
    // The ExportDialog order: stopForShutdown in the destructor body, then
    // QObject deletes the QProcess children. Nothing may arrive in between.
    int deliveries = 0;
    {
        Owner owner;
        auto *process = new QProcess(&owner);
        QObject::connect(process, &QProcess::errorOccurred, &owner,
                         [&deliveries](QProcess::ProcessError) { ++deliveries; });
        process->start(QStringLiteral("sleep"), {QStringLiteral("30")});
        assert(process->waitForStarted(5000));
        ProcessShutdown::stopForShutdown(&owner, {process});
    }
    assert(deliveries == 0);
    std::cerr << "PASS: destroying the owner after shutdown delivers nothing\n";
}

void testNullAndIdleProcessesAreSkipped()
{
    Owner owner;
    auto *idle = new QProcess(&owner);
    owner.watch(idle);

    ProcessShutdown::stopForShutdown(&owner, {nullptr, idle});

    assert(idle->state() == QProcess::NotRunning);
    assert(owner.deliveries == 0);
    std::cerr << "PASS: null and idle processes are skipped\n";
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testARunningProcessIsStoppedWithoutReachingItsOwner();
    testAProcessThatIgnoresSigintIsKilledAfterTheGrace();
    testDestroyingTheOwnerAfterShutdownDeliversNothing();
    testNullAndIdleProcessesAreSkipped();
    return 0;
}
