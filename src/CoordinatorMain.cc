/* Copyright (c) 2010-2012 Stanford University
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

#include "Common.h"
#include "CoordinatorService.h"
#include "ShortMacros.h"
#include "OptionParser.h"
#include "PingService.h"
#include "ServerId.h"
#include "ServiceManager.h"
#include "TableManager.h"
#include "TransportManager.h"
#include "PortAlarm.h"

/**
 * \file
 * This file provides the main program for the RAMCloud cluster coordinator.
 */

int
main(int argc, char *argv[])
{
    using namespace RAMCloud;
    Logger::installCrashBacktraceHandlers();
    string localLocator("???");
    uint32_t deadServerTimeout;
    string logCabinLocator("testing");
    Context context(true);
    CoordinatorServerList serverList(&context);
    TableManager tableManager(&context);
    try {
        OptionsDescription coordinatorOptions("Coordinator");
        coordinatorOptions.add_options()
            ("deadServerTimeout,d",
             ProgramOptions::value<uint32_t>(&deadServerTimeout)->
                default_value(250),
            "Number of milliseconds to wait for a potentially dead server to "
            "show signs of life before declaring it as crashed. The longer the "
            "timeout, the slower real crashes are responded to. The shorter "
            "the timeout, the greater the chance is of falsely deciding a "
            "machine is down when it's not.")
            ("logCabinLocator,z",
             ProgramOptions::value<string>(&logCabinLocator),
             "Locator where the LogCabin cluster can be contacted");

        OptionParser optionParser(coordinatorOptions, argc, argv);

        // Log all the command-line arguments.
        string args;
        for (int i = 0; i < argc; i++) {
            if (i != 0)
                args.append(" ");
            args.append(argv[i]);
        }
        LOG(NOTICE, "Command line: %s", args.c_str());

        pinAllMemory();
        localLocator = optionParser.options.getCoordinatorLocator();
        context.transportManager->setSessionTimeout(
                optionParser.options.getSessionTimeout());
        context.transportManager->initialize(localLocator.c_str());
        localLocator = context.transportManager->
                                getListeningLocatorsString();
        LOG(NOTICE, "coordinator: Listening on %s", localLocator.c_str());
        LOG(NOTICE, "PortTimeOut=%d", optionParser.options.getPortTimeout());

        // Set PortTimeout and start portTimer
        context.portAlarmTimer->setPortTimeout(
                optionParser.options.getPortTimeout());

        CoordinatorService coordinatorService(&context,
                                              deadServerTimeout,
                                              logCabinLocator);
        context.coordinatorService = &coordinatorService;
        context.serviceManager->addService(coordinatorService,
                                           WireFormat::COORDINATOR_SERVICE);
        PingService pingService(&context);
        context.serviceManager->addService(pingService,
                                           WireFormat::PING_SERVICE);

        Dispatch& dispatch = *context.dispatch;
        while (true) {
            dispatch.poll();
        }
        return 0;
    } catch (const std::exception& e) {
        LOG(ERROR, "Fatal error in coordinator at %s: %s",
            localLocator.c_str(), e.what());
        return 1;
    } catch (...) {
        LOG(ERROR, "Unknown fatal error in coordinator at %s",
            localLocator.c_str());
        return 1;
    }
}
